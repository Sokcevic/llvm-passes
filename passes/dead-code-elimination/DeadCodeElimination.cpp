#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h") // LLVM >= 23
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE "remove-dead-code"

using namespace llvm;

namespace {
struct DeadCodeEliminationPass : PassInfoMixin<DeadCodeEliminationPass> {

  static bool deleteTriviallyDead(Function &F) {
    SmallVector<Instruction *, 64> Work;
    Work.reserve(64);

    for (Instruction &I : instructions(F))
      Work.push_back(&I);

    bool Changed = false;

    while (!Work.empty()) {
      Instruction *I = Work.pop_back_val();
      if (!I || I->getParent() == nullptr)
        continue;

      if (!isInstructionTriviallyDead(I))
        continue;

      SmallVector<Value *, 8> Ops(I->operands());
      I->eraseFromParent();
      Changed = true;

      for (Value *V : Ops) {
        if (auto *OpI = dyn_cast_or_null<Instruction>(V))
          Work.push_back(OpI);
      }
    }

    return Changed;
  }

  static bool removeDeadStackStores(Function &F) {
    bool Changed = false;
    SmallVector<StoreInst *, 32> StoresToErase;

    for (Instruction &I : instructions(F)) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;

      bool HasLoad = false;
      bool Escapes = false;

      SmallVector<StoreInst *, 8> Stores;

      for (User *U : AI->users()) {
        if (isa<LoadInst>(U)) {
          HasLoad = true;
          continue;
        }
        if (auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() != AI) {
            Escapes = true;
            break;
          }
          Stores.push_back(SI);
          continue;
        }

        Escapes = true;
        break;
      }

      if (Escapes || HasLoad)
        continue;

      for (StoreInst *SI : Stores)
        StoresToErase.push_back(SI);
    }

    for (StoreInst *SI : StoresToErase) {
      SI->eraseFromParent();
      Changed = true;
    }

    return Changed;
  }

  static bool simplifySameSuccessorCondBr(Function &F) {
    bool Changed = false;

    for (BasicBlock &BB : F) {
      auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
      if (!BI || !BI->isConditional())
        continue;

      BasicBlock *S0 = BI->getSuccessor(0);
      BasicBlock *S1 = BI->getSuccessor(1);
      if (S0 != S1)
        continue;

      BasicBlock *Succ = S0;
      Value *Cond = BI->getCondition();

      SmallVector<PHINode *, 8> PHIsNeedingFix;
      bool CanSimplify = true;

      for (Instruction &I : *Succ) {
        auto *PN = dyn_cast<PHINode>(&I);
        if (!PN)
          break;

        SmallVector<unsigned, 2> Idxs;
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
          if (PN->getIncomingBlock(i) == &BB)
            Idxs.push_back(i);

        if (Idxs.empty())
          continue;

        if (Idxs.size() != 2) {
          CanSimplify = false;
          break;
        }

        PHIsNeedingFix.push_back(PN);
      }

      if (!CanSimplify)
        continue;

      IRBuilder<> B(BI);

      for (PHINode *PN : PHIsNeedingFix) {

        int i0 = -1, i1 = -1;
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
          if (PN->getIncomingBlock(i) == &BB) {
            if (i0 == -1)
              i0 = (int)i;
            else {
              i1 = (int)i;
              break;
            }
          }
        }

        if (i0 == -1 || i1 == -1) {
          CanSimplify = false;
          break;
        }

        Value *V0 = PN->getIncomingValue((unsigned)i0);
        Value *V1 = PN->getIncomingValue((unsigned)i1);

        Value *Merged =
            (V0 == V1) ? V0
                       : B.CreateSelect(Cond, V0, V1, PN->getName() + ".sel");

        if (i0 < i1)
          std::swap(i0, i1);
        PN->removeIncomingValue((unsigned)i0, false);
        PN->removeIncomingValue((unsigned)i1, false);
        PN->addIncoming(Merged, &BB);
      }

      if (!CanSimplify)
        continue;

      B.CreateBr(Succ);
      BI->eraseFromParent();
      Changed = true;
    }

    return Changed;
  }

  static bool removeEmptyBranchBlocks(Function &F) {
    bool Changed = false;
    bool Local = true;

    while (Local) {
      Local = false;

      for (auto It = F.begin(), End = F.end(); It != End;) {
        BasicBlock *BB = &*It++;
        if (BB == &F.getEntryBlock())
          continue;

        if (pred_empty(BB))
          continue;

        auto *Br = dyn_cast<BranchInst>(BB->getTerminator());
        if (!Br || !Br->isUnconditional())
          continue;

        bool HasOtherInsts = false;
        for (Instruction &I : *BB) {
          if (!isa<PHINode>(&I) && &I != Br) {
            HasOtherInsts = true;
            break;
          }
        }
        if (HasOtherInsts)
          continue;

        if (TryToSimplifyUncondBranchFromEmptyBlock(BB)) {
          Changed = true;
          Local = true;
          break;
        }
      }
    }

    return Changed;
  }

  static bool mergeIntoOnlyPred(Function &F) {
    bool Changed = false;

    bool Local = true;
    while (Local) {
      Local = false;

      for (auto It = F.begin(), End = F.end(); It != End;) {
        BasicBlock *BB = &*It++;
        if (BB == &F.getEntryBlock())
          continue;

        if (!BB->hasNPredecessors(1))
          continue;

        BasicBlock *Pred = *pred_begin(BB);
        auto *PT = Pred->getTerminator();

        if (PT->getNumSuccessors() != 1 || PT->getSuccessor(0) != BB)
          continue;

        MergeBasicBlockIntoOnlyPred(BB);
        Changed = true;
        Local = true;
        break;
      }
    }

    return Changed;
  }

  static bool removeDeadAllocaSlots(Function &F) {
    bool Changed = false;

    SmallVector<AllocaInst *, 16> Allocas;
    for (Instruction &I : instructions(F))
      if (auto *AI = dyn_cast<AllocaInst>(&I))
        Allocas.push_back(AI);

    SmallVector<Instruction *, 64> ToErase;

    for (AllocaInst *AI : Allocas) {

      bool Escapes = false;
      SmallVector<LoadInst *, 16> Loads;
      SmallVector<StoreInst *, 16> Stores;

      for (User *U : AI->users()) {
        if (auto *LI = dyn_cast<LoadInst>(U)) {
          Loads.push_back(LI);
          continue;
        }
        if (auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() != AI) {
            Escapes = true;
            break;
          }
          Stores.push_back(SI);
          continue;
        }
        Escapes = true;
        break;
      }
      if (Escapes)
        continue;

      bool AnyLiveLoad = false;
      for (LoadInst *LI : Loads) {
        for (User *U : LI->users()) {
          auto *UseI = dyn_cast<Instruction>(U);
          if (!UseI)
            continue;

          if (UseI->mayHaveSideEffects() || UseI->isTerminator()) {
            AnyLiveLoad = true;
            break;
          }
        }
        if (AnyLiveLoad)
          break;
      }

      if (AnyLiveLoad)
        continue;

      for (LoadInst *LI : Loads)
        ToErase.push_back(LI);
      for (StoreInst *SI : Stores)
        ToErase.push_back(SI);
    }

    for (Instruction *I : ToErase) {
      if (I->getParent()) {
        I->replaceAllUsesWith(PoisonValue::get(I->getType()));
        I->eraseFromParent();
        Changed = true;
      }
    }

    return Changed;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    bool Changed = false;

    bool LocalChanged = true;
    while (LocalChanged) {
      LocalChanged = false;

      LocalChanged |= removeDeadStackStores(F);
      LocalChanged |= removeDeadAllocaSlots(F);
      LocalChanged |= deleteTriviallyDead(F);

      LocalChanged |= removeEmptyBranchBlocks(F);
      LocalChanged |= simplifySameSuccessorCondBr(F);
      LocalChanged |= removeEmptyBranchBlocks(F);
      LocalChanged |= mergeIntoOnlyPred(F);

      LocalChanged |= deleteTriviallyDead(F);

      Changed |= LocalChanged;
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace

/// Registration
PassPluginLibraryInfo getPassPluginInfo() {
  const auto callback = [](PassBuilder &PB) {
    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM, auto) {
          if (Name == "dead-code-elimination") {
            FPM.addPass(DeadCodeEliminationPass());
            return true;
          }
          return false;
        });
  };
  return {LLVM_PLUGIN_API_VERSION, "DeadCodeEliminationPass",
          LLVM_VERSION_STRING, callback};
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}

#undef DEBUG_TYPE
