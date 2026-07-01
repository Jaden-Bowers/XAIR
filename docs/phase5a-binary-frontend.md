# Phase 5A: Binary-To-IR Frontend

Phase 5A starts the IR-generator side of the project. It lifts raw binary bytes
into XAIR one basic block at a time and reports exit metadata for a later CFG
builder to consume.

## Scope

- Raw byte image abstraction with base-address translation.
- Architecture selection for x86-64.
- Private Zydis-inspired decoded-instruction/operand boundary without vendoring
  Zydis source or generated tables.
- Basic-block lifter API that creates XAIR for one block.
- x86-64 subset:
  - REX prefix handling for 64-bit general-purpose registers.
  - `mov r64, imm64`.
  - register-register `mov`, `add`, `sub`, `cmp`, and `test`.
  - RIP-relative 64-bit `mov` loads and stores.
  - direct `jmp`, `jz`, `jnz`, and `ret`.
- Exit metadata for fallthrough, direct jump, direct conditional branch, return,
  and unsupported instruction.

## Non-goals

- No CFG construction.
- No PE/ELF/Mach-O parser.
- No full x86 decoder.
- No indirect branch recovery.
- No calls, stack modeling, SIMD, floating point, atomics, or privileged
  instructions.

## Design Notes

The generated XAIR block ends with `return`, not with CFG edges. This is
intentional. The IR-generator project should be benchmarkable independently from
CFG recovery. The result record contains target and fallthrough addresses for
the future CFG project.

The decoder stub uses a Zydis-like separation between decoding and semantic
lifting: byte parsing produces a small decoded instruction with typed operands,
and the lifter consumes that shape. This keeps the current dependency footprint
small while preserving a clean replacement point for a fuller decoder.

Architectural register reads become block parameters lazily. Register writes are
reported as output values. Memory is also introduced lazily; loads consume the
current memory token and stores produce a new one.

Unsupported instructions are explicit lift results, not approximations. The
block is still made verifier-valid so callers can inspect partial lifting
results without special cleanup.

## Next Step After This Phase

Expand the private decoder stub into a table-driven decoder behind the same
lifter API. Importing Zydis can remain a later option if the dependency tradeoff
becomes worthwhile. The current subset is for validating the frontend boundary
and benchmark harnesses, not for claiming complete x86 coverage.
