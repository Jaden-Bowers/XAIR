# XAIR

XAIR is a C-native cross-architecture analysis IR for binary symbolic execution,
taint tracking, and vulnerability discovery.

The design target is narrower than a compiler IR and more explicit than VEX:
typed SSA values, byte-addressed memory, explicit memory versions, lazy flag
summaries, and no opaque helper calls in optimized hot paths.

Current status: binary-to-IR frontend prototype with raw byte, PE, and ELF
entry lifting paths.

IR version: XAIR v0.1.0.

Implemented pieces:
- Core XAIR module, verifier, formatter, and metrics.
- Canonicalization pass.
- Concrete C executor baseline.
- VEX-shaped adapter layer.
- Raw x86-32 and x86-64 basic-block lifter for a benchmark-oriented
  instruction subset.
- `xair_lift_raw` tool for lifting raw byte blobs into XAIR text.
- `xair_lift_bin` tool for lifting the entry block from PE and ELF binaries.
- `xair_bench_lift` tool for in-process lift timing over benchmark cases.

## Design Boundary

This repository is the IR generator side of the project. CFG recovery,
symbolic execution, taint propagation, and benchmark orchestration are separate
layers that should consume this IR rather than being mixed into the frontend.

The current construction path is still a mutable bootstrap implementation. The
freeze boundary now finalizes the module enough for benchmarking: mutable growth
buffers are shrunk to their used size, per-block arrays are compacted, and the
construction value-numbering table is discarded. A later packed immutable module
format can replace this without changing the external v0 IR contract.

New code can build through `xair_builder`, then freeze the result with
`xair_builder_freeze` or `xair_module_freeze`. Frozen modules reject mutation
and are intended for verification, formatting, execution, and benchmarking.

Address arithmetic is intentionally explicit: integer `add`/`sub` are not valid
for address values. Address updates must use `addr_add`, `addr_sub`,
`int_to_addr`, and `addr_to_int`.

The v0 IR text format is covered by golden tests. Changes to opcode semantics,
type rules, or printed IR should be treated as versioned changes.

Construction performs structural value numbering for constants, unary nodes,
binary nodes, loads, and stores. Constants are pooled globally across blocks.
Non-constant expressions remain block-local, which keeps the current dominance
model simple and avoids unsafe global CSE.

Lazy flags cover add, sub, logic, and shift-left summaries, with extractors for
ZF, CF, OF, SF, PF, and AF.

The x86 lifter consumes a decoder backend interface. The current backend is the
private `x86_stub` decoder for x86-32 and x86-64, and future Zydis, XED, or
generated-table decoders can be added without rewriting the lifter.

The binary loader recognizes PE32, PE32+, ELF32, and ELF64 for x86 targets. It
copies the executable section or load segment containing the requested entry
address and lifts one basic block from that image slice. This is intentionally
not a full operating-system loader: relocations, imports, TLS, and full process
memory layout belong in the later CFG and execution layers.

For benchmark harnesses, XAIR exposes `xair_module_fingerprint`, JSON output
from `xair_lift_raw --json`, and a small corpus runner named `xair_case_run`.
The JSON output reports construction value-numbering separately from final
value-numbering so freeze-time table cleanup is visible. The runner accepts
key-value case files with raw bytes, base address, entry address, optional
register inputs, and optional memory bytes.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For strict local cleanup with MSVC, GCC, or Clang:

```sh
cmake -S . -B build-strict -DXAIR_STRICT_WARNINGS=ON
cmake --build build-strict
```

For optimized WSL benchmarks:

```sh
cmake -S . -B build-wsl-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DXAIR_STRICT_WARNINGS=ON
cmake --build build-wsl-release
ctest --test-dir build-wsl-release --output-on-failure
```

## Lift Tools

```sh
build/Debug/xair_lift_raw.exe [--json] [--arch x86_64|x86_32] <raw-binary> <base> <entry> [max-instructions]
build/Debug/xair_lift_bin.exe [--json] <pe-or-elf> [entry-va] [max-instructions]
build/Debug/xair_case_run.exe <case-file>
build/Debug/xair_bench_lift.exe --repeat 1000 --warmup 100 <case-file>...
```

`xair_lift_raw` expects a raw byte blob and explicit base and entry addresses.
`xair_lift_bin` reads a PE or ELF binary, detects x86-32 or x86-64, extracts
the entry section or segment, and prints lift metadata plus the generated XAIR
block. CFG construction is intentionally left for a later separate project that
consumes this frontend.

The frontend reports block input registers, output registers, memory tokens,
branch metadata, and generated XAIR so lift speed and IR inflation can be
benchmarked separately from CFG recovery.

`xair_bench_lift` is the preferred timing path. It keeps the process alive,
loads benchmark cases once, warms the case bytes, repeats lifting in a tight
loop, and reports JSONL rows with lift, canonicalize, freeze, fingerprint,
value-numbering, and final module metrics.

## Attribution

The private x86 decoder stub is shaped after Zydis' decoded-instruction and
typed-operand API style, but does not vendor Zydis source code or generated
tables. Zydis is MIT licensed by Florian Bernd, Joel Hoener, and contributors.
