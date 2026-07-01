# XAIR Phase Plan

This plan is based on the local research notes in `../kb/`, especially the deep
research report. Each phase has a small exit criterion so the implementation can
stay conservative and measurable.

## Phase 1: Core IR

Goal: define the minimal C IR container that later lifters, optimizers, and
executors can share.

Scope:
- Typed values: integers, addresses, flags, memory tokens, labels.
- Basic blocks with block parameters.
- Value-producing operations with dense IDs.
- Terminators for jump, conditional branch, return, trap, and fault.
- Verifier for type consistency and block argument consistency.
- Formatter for tests and debugging.

Exit criterion:
- Unit tests build and pass.
- A flags-heavy block and a memory-versioned block verify successfully.
- A deliberate type mismatch fails verification.

## Phase 2: Canonicalization

Goal: make lifted IR compact before execution.

Scope:
- Stable operand ordering for commutative ops.
- Width-aware constant folding.
- Dead value, dead flag, dead memory-token pruning.
- Dense ID renumbering.

Exit criterion:
- Canonical forms are deterministic.
- Dead flag summaries are removed when no flag is extracted.
- IR inflation can be measured per machine instruction.

## Phase 3: Concrete Executor

Goal: execute verified XAIR without Python in the hot path.

Scope:
- Direct C interpreter over contiguous op records.
- Native fast path for 1, 8, 16, 32, and 64-bit integer values.
- Copy-on-write memory pages.
- Explicit fault/trap outcomes.

Exit criterion:
- Core arithmetic, branch, load, and store tests pass.
- Execution throughput is benchmarked independently from lifting and solving.

## Phase 4: VEX Adapter

Goal: bootstrap compatibility and create an oracle before native lifters are
complete.

Scope:
- Convert VEX temporaries, `GET`, `PUT`, loads, stores, exits, and common IROps.
- Convert VEX dirty/ccall cases only through declared XAIR intrinsics.

Exit criterion:
- Smoke corpus produces equivalent architectural state against VEX execution.

## Phase 5: Native x86-64 Lifter

Goal: outperform VEX on the first native architecture.

### Phase 5A: Binary-To-IR Frontend Boundary

Goal: make the IR generator independently usable before CFG recovery exists.

Scope:
- Raw byte image abstraction.
- x86-64 basic-block lift API.
- Standalone raw lifting executable.
- Exit metadata for return, direct jump, direct conditional branch, fallthrough,
  and unsupported instruction.

Exit criterion:
- A raw byte blob can be lifted into verifier-valid XAIR.
- Lifting can be benchmarked separately from CFG construction.

### Phase 5B: Full Native x86-64 Lifter

Scope:
- Zydis-inspired or table-driven decoder, keeping direct Zydis import optional.
- REX, ModR/M, SIB, RIP-relative addressing.
- Arithmetic, logic, moves, branches, calls, returns, loads, stores, `CMOVcc`.
- Lazy flags through `flags_*` summaries.

Exit criterion:
- Differential tests agree with VEX and hardware/QEMU on bounded instruction
  streams.
- Lift latency and IR inflation improve over VEX on the benchmark set.

## Phase 6: Symbolic And Taint Backends

Goal: support the vulnerability finder directly.

Scope:
- Canonical expression DAGs for SMT lowering.
- Byte-addressed memory arrays with region-aware symbolic addresses.
- Shadow taint labels with an 8-bit fast path and interned label-set fallback.

Exit criterion:
- Taint propagation and symbolic query counts are measured separately.
- Symbolic pointer tests avoid uncontrolled address forking on common cases.
