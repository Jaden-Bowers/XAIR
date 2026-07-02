# XAIR

XAIR is a C-native cross-architecture analysis IR for binary symbolic execution,
taint tracking, and vulnerability discovery.

The design target is narrower than a compiler IR and more explicit than VEX:
typed SSA values, byte-addressed memory, explicit memory versions, lazy flag
summaries, and no opaque helper calls in optimized hot paths.

Current status: Phase 5B benchmark-ready binary-to-IR frontend prototype.

IR version: XAIR v0.1.0.

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

The current construction path is still a mutable bootstrap implementation, but
it now has an explicit freeze boundary. The remaining core-IR work is to move
more variable-length data into arenas or slabs and keep improving structural
value numbering before expanding symbolic or taint backends.

New code can build through `xair_builder`, then freeze the result with
`xair_builder_freeze` or `xair_module_freeze`. Frozen modules reject mutation
and are intended for verification, formatting, execution, and benchmarking.

Address arithmetic is intentionally explicit: integer `add`/`sub` are not valid
for address values. Address updates must use `addr_add`, `addr_sub`,
`int_to_addr`, and `addr_to_int`.

The v0 IR text format is covered by golden tests. Changes to opcode semantics,
type rules, or printed IR should be treated as versioned changes.

Construction performs local structural value numbering for equivalent constants,
unary nodes, binary nodes, loads, and stores. This keeps repeated expressions
from inflating the IR before canonicalization runs.

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
