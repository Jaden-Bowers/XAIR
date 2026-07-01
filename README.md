# XAIR

XAIR is a C-native cross-architecture analysis IR for binary symbolic execution,
taint tracking, and vulnerability discovery.

The design target is narrower than a compiler IR and more explicit than VEX:
typed SSA values, byte-addressed memory, explicit memory versions, lazy flag
summaries, and no opaque helper calls in optimized hot paths.

Current status: Phase 1 core IR prototype.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

