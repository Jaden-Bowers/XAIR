# Phase 4: VEX Adapter

Phase 4 adds a conservative compatibility layer for building XAIR from
VEX-shaped input. This is the bootstrap path for differential testing while
native lifters are still incomplete.

## Scope

- Track VEX temporaries as XAIR value ids.
- Model `GET` and `PUT` with register-offset mappings.
- Track the current memory token for loads and stores.
- Lower common integer IROps by VEX-style names such as `Iop_Add64`,
  `Iop_CmpEQ64`, `Iop_CmpLT64S`, `Iop_32Uto64`, and `Iop_64to32`.
- Lower VEX-style conditional exits by splitting the current XAIR block and
  passing live adapter state into the false-path continuation.

## Non-goals

- No direct dependency on libVEX or PyVEX yet.
- No parser for textual VEX dumps.
- No dirty helpers, CCalls, floating point, SIMD, atomics, or architecture
  register-layout database.
- No liveness pruning across exits. All known adapter state is transferred to
  the continuation in this phase.

## Design Notes

The adapter deliberately uses only the public XAIR builder API. That keeps the
compatibility layer honest: if the adapter cannot express a VEX construct
without reaching into private IR storage, the IR surface probably needs a real
extension.

`GET` creates a block parameter the first time a register offset is read. `PUT`
updates the offset mapping. This gives tests and future frontends a simple way
to seed architectural state through executor parameters.

VEX exits are side exits: when the condition is false, execution continues with
later statements in the same IRSB. XAIR blocks cannot use predecessor-local
values directly, so the adapter creates a continuation block and passes current
temporaries, registers, and memory as block arguments.

## Exit Criteria

- A VEX-shaped arithmetic/register sequence verifies and executes through XAIR.
- Loads and stores use the adapter-managed memory token.
- Conditional exits create verifier-valid continuation blocks.
- Unsupported IROps fail at adapter build time instead of being approximated.
