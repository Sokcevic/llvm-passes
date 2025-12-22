#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h") // LLVM >= 23
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "memory-safety"

using namespace llvm;

namespace {
struct MemorySafetyPass : PassInfoMixin<MemorySafetyPass> {
  static bool shouldSkipFunction(Function &F) {
    return F.getName().starts_with("__runtime_");
  }
  static bool isGlobalPointer(Value *Ptr) {
    Ptr = Ptr->stripPointerCasts();
    return isa<GlobalVariable>(Ptr);
  }

  static FunctionCallee getOrInsertCheck(Module &M) {
    LLVMContext &C = M.getContext();
    Type *VoidTy = Type::getVoidTy(C);
    Type *PtrTy = PointerType::get(C, 0);
    Type *I64Ty = Type::getInt64Ty(C);
    return M.getOrInsertFunction("__runtime_check_addr", VoidTy, PtrTy, I64Ty);
  }

  static FunctionCallee getOrInsertMalloc(Module &M) {
    LLVMContext &C = M.getContext();
    Type *PtrTy = PointerType::get(C, 0);
    Type *I64Ty = Type::getInt64Ty(C);
    return M.getOrInsertFunction("__runtime_malloc", PtrTy, I64Ty);
  }

  static FunctionCallee getOrInsertFree(Module &M) {
    LLVMContext &C = M.getContext();
    Type *VoidTy = Type::getVoidTy(C);
    Type *PtrTy = PointerType::get(C, 0);
    return M.getOrInsertFunction("__runtime_free", VoidTy, PtrTy);
  }

  static FunctionCallee getOrInsertStackAlloc(Module &M) {
    LLVMContext &C = M.getContext();
    Type *VoidTy = Type::getVoidTy(C);
    Type *PtrTy = PointerType::get(C, 0);
    Type *I64Ty = Type::getInt64Ty(C);
    return M.getOrInsertFunction("__runtime_stack_alloc", VoidTy, PtrTy, I64Ty);
  }

  static FunctionCallee getOrInsertStackUnpoison(Module &M) {
    LLVMContext &C = M.getContext();
    Type *VoidTy = Type::getVoidTy(C);
    Type *PtrTy = PointerType::get(C, 0);
    Type *I64Ty = Type::getInt64Ty(C);
    return M.getOrInsertFunction("__runtime_stack_unpoison", VoidTy, PtrTy,
                                 I64Ty);
  }

  static FunctionCallee getOrInsertStackPoison(Module &M) {
    LLVMContext &C = M.getContext();
    Type *VoidTy = Type::getVoidTy(C);
    Type *PtrTy = PointerType::get(C, 0);
    Type *I64Ty = Type::getInt64Ty(C);
    return M.getOrInsertFunction("__runtime_stack_poison", VoidTy, PtrTy,
                                 I64Ty);
  }

  static bool isTrackedPointer(Value *Ptr) {
    SmallPtrSet<Value *, 16> Visited;
    return isTrackedPointerImpl(Ptr, Visited);
  }

  static bool isTrackedPointerImpl(Value *Ptr,
                                   SmallPtrSetImpl<Value *> &Visited) {
    Ptr = Ptr->stripPointerCasts();

    if (!Visited.insert(Ptr).second)
      return false;

    if (isa<AllocaInst>(Ptr))
      return true;
    if (isa<Argument>(Ptr))
      return true;

    if (auto *CB = dyn_cast<CallBase>(Ptr)) {
      if (Function *F = CB->getCalledFunction())
        return F->getName() == "__runtime_malloc";
    }

    if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr))
      return isTrackedPointerImpl(GEP->getPointerOperand(), Visited);

    if (auto *PN = dyn_cast<PHINode>(Ptr)) {
      for (Value *Incoming : PN->incoming_values())
        if (isTrackedPointerImpl(Incoming, Visited))
          return true;
      return false;
    }

    return false;
  }

  static bool alreadyChecked(Instruction *I, Value *I8Ptr, uint64_t Size) {
#if LLVM_VERSION_MAJOR >= 23
    Instruction *Prev = I->getPrevNode(); // debug info lives in records now
#else
    Instruction *Prev = I->getPrevNonDebugInstruction();
#endif
    if (!Prev)
      return false;

    auto *Call = dyn_cast<CallBase>(Prev);
    if (!Call)
      return false;

    Function *Callee = Call->getCalledFunction();
    if (!Callee || Callee->getName() != "__runtime_check_addr")
      return false;

    if (Call->arg_size() != 2)
      return false;

    Value *PrevPtr = Call->getArgOperand(0)->stripPointerCasts();
    Value *ThisPtr = I8Ptr->stripPointerCasts();

    if (PrevPtr != ThisPtr)
      return false;

    auto *PrevSz = dyn_cast<ConstantInt>(Call->getArgOperand(1));
    if (!PrevSz)
      return false;

    return PrevSz->getZExtValue() == Size;
  }

  static bool replaceMallocFree(Function &F, Module &M) {
    LLVMContext &C = M.getContext();
    FunctionCallee RtMalloc = getOrInsertMalloc(M);
    FunctionCallee RtFree = getOrInsertFree(M);

    bool Changed = false;

    SmallVector<CallBase *, 32> Calls;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        Calls.push_back(CB);

    for (CallBase *CB : Calls) {
      if (!CB || !CB->getParent())
        continue;

      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        continue;

      StringRef Name = Callee->getName();
      IRBuilder<> B(CB);

      if (Name == "malloc") {
        if (CB->arg_size() != 1)
          continue;

        Value *Sz = CB->getArgOperand(0);

        Value *Sz64 = Sz;
        if (!Sz->getType()->isIntegerTy(64))
          Sz64 = B.CreateZExtOrTrunc(Sz, Type::getInt64Ty(C));

        CallInst *NewCall = B.CreateCall(RtMalloc, {Sz64});

        Value *Res = NewCall;
        if (Res->getType() != CB->getType())
          Res = B.CreateBitCast(Res, CB->getType());

        CB->replaceAllUsesWith(Res);
        CB->eraseFromParent();
        Changed = true;
        continue;
      }

      if (Name == "free") {
        if (CB->arg_size() != 1)
          continue;

        Value *Ptr = CB->getArgOperand(0);
        if (!Ptr->getType()->isPointerTy())
          continue;

        Value *I8Ptr = Ptr;
        if (Ptr->getType() != PointerType::get(C, 0))
          I8Ptr = B.CreateBitCast(Ptr, PointerType::get(C, 0));

        B.CreateCall(RtFree, {I8Ptr});
        CB->eraseFromParent();
        Changed = true;
        continue;
      }
    }

    return Changed;
  }

  static bool instrumentLoadsStores(Function &F, Module &M) {
    LLVMContext &C = M.getContext();
    const DataLayout &DL = M.getDataLayout();

    FunctionCallee CheckFn = getOrInsertCheck(M);
    bool Changed = false;

    SmallVector<Instruction *, 128> Insts;
    for (Instruction &I : instructions(F))
      Insts.push_back(&I);

    for (Instruction *I : Insts) {
      if (!I || !I->getParent())
        continue;

      Value *Ptr = nullptr;
      Type *AccessTy = nullptr;

      if (auto *LI = dyn_cast<LoadInst>(I)) {
        Ptr = LI->getPointerOperand();
        AccessTy = LI->getType();
      } else if (auto *SI = dyn_cast<StoreInst>(I)) {
        Ptr = SI->getPointerOperand();
        AccessTy = SI->getValueOperand()->getType();
      } else {
        continue;
      }
      if (!Ptr || !AccessTy)
        continue;
      if (!isTrackedPointer(Ptr))
        continue;
      if (isGlobalPointer(Ptr))
        continue;

      uint64_t Size = DL.getTypeStoreSize(AccessTy);
      if (Size == 0)
        continue;

      IRBuilder<> B(I);
      Value *I8Ptr = Ptr;
      if (Ptr->getType() != PointerType::get(C, 0))
        I8Ptr = B.CreateBitCast(Ptr, PointerType::get(C, 0));

      if (alreadyChecked(I, I8Ptr, Size))
        continue;

      Value *Sz64 = ConstantInt::get(Type::getInt64Ty(C), Size);
      B.CreateCall(CheckFn, {I8Ptr, Sz64});
      Changed = true;
    }

    return Changed;
  }

  static bool instrumentStackAllocas(Function &F, Module &M) {
    LLVMContext &C = M.getContext();
    const DataLayout &DL = M.getDataLayout();
    FunctionCallee StackAllocFn = getOrInsertStackAlloc(M);

    bool Changed = false;

    SmallVector<AllocaInst *, 32> Allocas;
    for (Instruction &I : instructions(F))
      if (auto *AI = dyn_cast<AllocaInst>(&I))
        Allocas.push_back(AI);

    for (AllocaInst *AI : Allocas) {
      if (!AI || !AI->getParent())
        continue;

      if (AI->getName().contains(".rt"))
        continue;

      auto *ArrSzC = dyn_cast<ConstantInt>(AI->getArraySize());
      if (!ArrSzC)
        continue;

      Type *ElemTy = AI->getAllocatedType();
      uint64_t ElemSize = DL.getTypeAllocSize(ElemTy);
      uint64_t Count = ArrSzC->getZExtValue();
      uint64_t UserSize = ElemSize * Count;
      if (UserSize == 0)
        continue;

      uint64_t TotalSize = UserSize + 32;

      IRBuilder<> B(AI);

      Type *I8Ty = Type::getInt8Ty(C);
      Type *I64Ty = Type::getInt64Ty(C);

      AllocaInst *Base = B.CreateAlloca(
          I8Ty, ConstantInt::get(I64Ty, TotalSize), AI->getName() + ".rtbase");

      Base->setAlignment(Align(16));

      Value *UserI8 = B.CreateInBoundsGEP(
          I8Ty, Base, ConstantInt::get(I64Ty, 16), AI->getName() + ".rtuser");

      Value *UserTyped = B.CreateBitCast(UserI8, AI->getType());

      AI->replaceAllUsesWith(UserTyped);

      B.CreateCall(StackAllocFn, {UserI8, ConstantInt::get(I64Ty, UserSize)});

      AI->eraseFromParent();

      Changed = true;
    }

    return Changed;
  }

  // Returns {ptr, size} of a lifetime marker. Newer LLVM drops the size
  // operand, so it is recovered from the redzone-padded alloca made above.
  static std::pair<Value *, Value *> lifetimeArgs(CallBase *CB) {
    if (CB->arg_size() == 2)
      return {CB->getArgOperand(1), CB->getArgOperand(0)};

    Value *Ptr = CB->getArgOperand(0);
    Value *Size = nullptr;
    if (auto *Base = dyn_cast<AllocaInst>(Ptr->stripInBoundsConstantOffsets()))
      if (auto *N = dyn_cast<ConstantInt>(Base->getArraySize()))
        if (Base->getName().ends_with(".rtbase"))
          Size = ConstantInt::get(Type::getInt64Ty(CB->getContext()),
                                  N->getZExtValue() - 32);
    return {Ptr, Size};
  }

  static bool instrumentStackLifetime(Function &F, Module &M) {
    FunctionCallee UnpoisonFn = getOrInsertStackUnpoison(M);
    FunctionCallee PoisonFn = getOrInsertStackPoison(M);

    bool Changed = false;
    SmallVector<Instruction *, 16> Work;

    for (Instruction &I : instructions(F))
      Work.push_back(&I);

    for (Instruction *I : Work) {
      auto *CB = dyn_cast<CallBase>(I);
      if (!CB)
        continue;

      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        continue;

      IRBuilder<> B(CB);

      if (Callee->getName().starts_with("llvm.lifetime.start")) {
        auto [Ptr, Size] = lifetimeArgs(CB);
        if (Size)
          B.CreateCall(UnpoisonFn, {Ptr, Size});
        CB->eraseFromParent();
        Changed = true;
        continue;
      }

      if (Callee->getName().starts_with("llvm.lifetime.end")) {
        auto [Ptr, Size] = lifetimeArgs(CB);
        if (Size)
          B.CreateCall(PoisonFn, {Ptr, Size});
        CB->eraseFromParent();
        Changed = true;
        continue;
      }
    }

    return Changed;
  }

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    errs() << "Running MemorySafetyPass on function " << F.getName() << "\n";
    if (shouldSkipFunction(F))
      return PreservedAnalyses::all();

    Module &M = *F.getParent();

    bool Changed = false;
    Changed |= instrumentStackAllocas(F, M);
    Changed |= replaceMallocFree(F, M);
    Changed |= instrumentLoadsStores(F, M);
    Changed |= instrumentStackLifetime(F, M);

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};
} // namespace

/// Registration
PassPluginLibraryInfo getPassPluginInfo() {
  const auto callback = [](PassBuilder &PB) {
    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM, auto) {
          if (Name == "memory-safety") {
            FPM.addPass(MemorySafetyPass());
            return true;
          }
          return false;
        });
  };
  return {LLVM_PLUGIN_API_VERSION, "MemorySafetyPass", LLVM_VERSION_STRING,
          callback};
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}

#undef DEBUG_TYPE
