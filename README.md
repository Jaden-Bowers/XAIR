# XAIR

XAIR is a C-native cross-architecture analysis IR for binary symbolic execution,
taint tracking, and vulnerability discovery.

The design target is narrower than a compiler IR and more explicit than VEX:
typed SSA values, byte-addressed memory, explicit memory versions, lazy flag
summaries, and no opaque helper calls in optimized hot paths.

Current status: Phase 5B benchmark-ready binary-to-IR frontend prototype.

Implemented pieces:
- Core XAIR module, verifier, formatter, and metrics.
- Canonicalization pass.
- Concrete C executor baseline.
- VEX-shaped adapter layer.
- Raw x86-64 basic-block lifter for a benchmark-oriented instruction subset.
- `xair_lift_raw` tool for lifting raw byte blobs into XAIR text.

## Design Boundary

This repository is the IR generator side of the project. CFG recovery,
symbolic execution, taint propagation, and benchmark orchestration are separate
layers that should consume this IR rather than being mixed into the frontend.

The current module builder is still a mutable bootstrap implementation. The
next core-IR work should freeze an immutable module format, add arena/slab
allocation, and introduce structural value numbering before expanding symbolic
or taint backends.

Address arithmetic is intentionally explicit: integer `add`/`sub` are not valid
for address values. Address updates must use `addr_add`, `addr_sub`,
`int_to_addr`, and `addr_to_int`.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Raw Lift Tool

```sh
build/Debug/xair_lift_raw.exe <raw-binary> <base> <entry> [max-instructions]
```

This tool expects a raw byte blob, not a PE/ELF/Mach-O file. It prints lift
metadata and the generated XAIR block. CFG construction is intentionally left
for a later separate project that consumes this frontend.

The frontend reports block input registers, output registers, memory tokens,
branch metadata, and generated XAIR so lift speed and IR inflation can be
benchmarked separately from CFG recovery.

## Attribution

The private x86 decoder stub is shaped after Zydis' decoded-instruction and
typed-operand API style, but does not vendor Zydis source code or generated
tables. Zydis is MIT licensed by Florian Bernd, Joel Hoener, and contributors.
