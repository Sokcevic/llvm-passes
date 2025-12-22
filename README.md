# llvm-passes

Two out-of-tree LLVM passes built as `opt` plugins for the new pass manager:

1. **`dead-code-elimination`**: a dead code and CFG cleanup pass.
2. **`memory-safety`**: a small AddressSanitizer-style instrumentation pass, with a shadow-memory runtime written in Rust.

Neither pass relies on other LLVM transformation passes (no `instcombine`, `simplifycfg`, etc.). All rewriting is done by hand on the IR.

## `dead-code-elimination`

[`passes/dead-code-elimination/DeadCodeElimination.cpp`](passes/dead-code-elimination/DeadCodeElimination.cpp)

The pass runs these transformations repeatedly until it reaches a fixed point:

- **Trivially dead instructions:** a worklist removes instructions that have no uses and no side effects, then re-checks their operands, which may have become dead in turn.
- **Dead stack stores:** removes stores to non-escaping `alloca`s that are never loaded, followed by the dead `alloca` slots themselves.
- **Redundant branches:** turns conditional branches whose two targets are the same block into unconditional branches.
- **Empty blocks:** removes blocks that only forward to a successor, and fixes up the PHI nodes.
- **Block merging:** merges a block into its single predecessor when that predecessor has exactly one successor.

```sh
opt -load-pass-plugin passes/dead-code-elimination/build/libDeadCodeElimination.so \
    -passes=dead-code-elimination -S in.ll -o out.ll
```

## `memory-safety`

[`passes/memory-safety/MemorySafety.cpp`](passes/memory-safety/MemorySafety.cpp) · runtime: [`passes/memory-safety/src/lib.rs`](passes/memory-safety/src/lib.rs)

The pass instruments a program so that invalid memory accesses abort at runtime with `Illegal memory access`. It detects:

- **heap buffer overflow/underflow:** `malloc`/`free` are redirected to runtime versions that pad every allocation with 16-byte redzones on both sides
- **heap use-after-free:** freed memory is poisoned
- **stack buffer overflow:** every `alloca` is rewritten into a 16-byte-aligned slot with redzones, and `llvm.lifetime.start/end` markers poison and unpoison it

Every load and store through a tracked pointer gets a `__runtime_check_addr(ptr, size)` call. Back-to-back duplicate checks on the same address are skipped.

The runtime keeps **ASan-style shadow memory**: 1 shadow byte per 8 application bytes (scale 3), allocated lazily in 64 KiB chunks. This keeps checks cheap and memory overhead low even with many large allocations.

```sh
clang -g -O1 -S -emit-llvm prog.c -o prog.ll
opt -load-pass-plugin passes/memory-safety/build/libMemorySafety.so \
    -passes=memory-safety -S prog.ll -o prog-san.ll
clang prog-san.ll passes/memory-safety/build/libMemorySafetyRuntime.a -lstdc++ -o prog-san
./prog-san
```

## Build

Requires LLVM development headers (tested with LLVM 23; older LLVM versions are handled through version guards), CMake, Ninja, `clang` and a Rust toolchain for the runtime.

```sh
make            # builds both passes and the runtime library
```

If CMake can't find LLVM, pass `-DLT_LLVM_INSTALL_DIR=/path/to/llvm`.

## Tests

```sh
make check
```

- `tests/dce`: `lit` + `FileCheck` tests covering increasingly complex dead code and CFG patterns (needs `lit` and `FileCheck`).
- `tests/memory-safety`: C programs that are compiled, instrumented and run, checking detection of heap out-of-bounds, use-after-free and stack out-of-bounds accesses, with no false positives on valid accesses. A performance test checks that the instrumented program stays within time and memory limits.

## License

MIT. See [LICENSE](LICENSE). The build and test scaffolding is based on MIT-licensed code by Martin Fink.
