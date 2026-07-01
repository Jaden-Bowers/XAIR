# Phase 3: Concrete Executor

Phase 3 adds a C execution baseline for verified XAIR. The executor is not the
final hot-path dispatch engine; it is the first correctness layer that lets the
project measure IR behavior without Python or angr in the loop.

## Scope

- Run basic blocks through a bounded interpreter.
- Support 1, 8, 16, 32, and 64-bit integer and address values.
- Execute arithmetic, bitwise, shifts, compares, casts, concatenation, lazy flag
  summaries, flag extracts, loads, stores, jumps, conditional branches, returns,
  traps, and faults.
- Maintain explicit memory-token ordering while backing concrete memory with
  page-sized byte storage.
- Return unsupported status for wide/vector/intrinsic-style cases that are not
  part of the current fast path.

## Non-goals

- No symbolic values.
- No taint labels.
- No host-code JIT or computed-goto dispatch.
- No vector/SIMD cold path.
- No architectural register file abstraction. Lifters and adapters pass values
  through block parameters and returns for now.

## Design Notes

The executor stores one concrete slot per XAIR value id. Block transitions copy
terminator argument values into the target block parameters. This mirrors SSA
block parameters and keeps phi-like behavior explicit.

Memory operations still consume and produce memory-token values, but the current
concrete backend applies stores to the executor memory pages immediately. The
token remains valuable because it preserves ordering in the IR and gives later
symbolic/taint backends a single contract to reuse.

The initial memory store is page based with deterministic lookup. A hash-indexed
page table can replace the lookup without changing the public API once benchmark
data shows where the cost sits.

## Exit Criteria

- Concrete arithmetic and flag-driven branches run through the executor.
- Loads observe prior stores through explicit memory versions.
- Infinite control flow is bounded by the step limit.
- Unsupported operations fail clearly instead of silently lowering precision.
