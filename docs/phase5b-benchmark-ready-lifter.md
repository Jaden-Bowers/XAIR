# Phase 5B: Benchmark-Ready Lifter Subset

Phase 5B makes the IR-generator project ready for a separate benchmark harness.
It does not claim full x86-64 coverage. The goal is to cover enough common
basic-block patterns that lift speed, IR inflation, and concrete execution can
be measured without immediately stopping on simple compiler output.

## Scope

- Explicit `int_to_addr` and `addr_to_int` IR casts for GPR-derived addresses.
- Lazy input-register metadata in lift results.
- ModR/M and SIB memory operand decoding for 64-bit forms in the supported
  subset.
- Stack-oriented instructions:
  - `push r64`
  - `pop r64`
- Additional integer instructions:
  - `nop`
  - `lea r64, mem`
  - `and`, `or`, `xor`
  - `add`, `sub`, `cmp` with sign-extended imm8/imm32
- Stack-frame style execution through the concrete executor.

## Still Out Of Scope

- Calls.
- Indirect branches.
- CFG construction.
- Full flags beyond the ZF path needed by `jz` and `jnz`.
- 8/16/32-bit GPR writes and partial-register semantics.
- SIMD, floating point, atomics, string ops, and privileged instructions.
- PE/ELF/Mach-O parsing.

## Benchmark Readiness

The frontend now reports:

- Lift end kind and branch targets.
- Input registers required by the block.
- Output registers produced by the block.
- Input and output memory tokens.
- Return values that allow the concrete executor to be used for small semantic
  checks.

This is enough for the next project step: build a harness that feeds the same
raw basic-block corpus into XAIR, VEX, and other candidate IR frontends, then
records lift time, IR size, and execution behavior separately.
