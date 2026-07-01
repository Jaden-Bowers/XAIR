# Phase 2 Canonicalization

Phase 2 implements a deliberately small canonicalization pass. It is not a
general optimizer yet; it prepares lifted blocks for later execution and
symbolic lowering.

## Pass Order

1. Verify the input module.
2. Fold local constants when every required operand is already a constant.
3. Order operands for commutative operations with a stable total order.
4. Mark values reachable from block terminators.
5. Retain block parameters as CFG interface values.
6. Remove dead operation results, including unused flag summaries and unused
   memory-token stores.
7. Compact operation and value arrays, then renumber dense IDs.
8. Verify the output module.

## Conservative Choices

- Block parameters are not pruned in this phase. They represent block interface
  contracts, so pruning them belongs in a later CFG-aware pass.
- Stores are dead only when their result memory token is not reachable. The IR
  treats memory effects as explicit token flow.
- Constant folding is limited to integer and address widths up to 64 bits.
- Shift folding is skipped when the shift amount is greater than or equal to the
  shifted width.
- Flag extraction can fold from constant `flags_add` and `flags_sub`, but flag
  summary values themselves are not represented as constants.

## Exit Criteria

- Running canonicalization twice produces the same formatted IR.
- Unused lazy flag summaries are removed.
- Unused memory-token stores are removed.
- The caller can compute operations per machine instruction for inflation
  tracking.
