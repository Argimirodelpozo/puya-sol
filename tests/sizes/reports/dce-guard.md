# The zero-divisor guard

The guard in `src/builder/eb/SolIntegerBuilder.cpp` preserves a required Solidity
failure when backend dead-code elimination removes an unused division/modulo
value. For `return (a / b) << 256`, the result is zero only if evaluating `a / b`
succeeds. Replacing the entire expression with zero incorrectly accepts `b == 0`.

Lowering captures operands once, in the selected evaluation order, and emits
an explicit nonzero-divisor assertion before the arithmetic. Conditional and
short-circuit lowering retain the assertion in its own evaluated branch. The
quotient/remainder may then be discarded without discarding the failure check.

This follows the pinned solc implementation: `overflowCheckedIntDivFunction`,
`wrappingIntDivFunction` and `intModFunction` in
[`YulUtilFunctions.cpp`](../../../solidity/libsolidity/codegen/YulUtilFunctions.cpp)
all emit an explicit zero-divisor panic. Division/modulo by zero still fails
inside Solidity `unchecked` blocks.

This is not a change to raw Yul/EVM arithmetic: `div`, `sdiv`, `mod` and `smod`
return zero for a zero divisor. Those operations use separate lowering.

Nor is it full EVM panic equivalence. The target assertion aborts the AVM
transaction; it does not return ABI-encoded `Panic(0x12)` data or reproduce
catchable EVM child-call failure. The assertion's `Panic 0x12` message is a
diagnostic, not a returndata payload. Existing call-failure adaptations remain
in [EVM_DIVERGENCE.md](../../../EVM_DIVERGENCE.md). The pinned Puya optimizer is
unchanged, and this fix does not prove every unused may-trap expression safe.

## Measured cost

Comparison: `e94bbc6218` to `6d940b1e43`, with the same current regression source
and Puya 5.10.1 backend. The six-function `dce_reverting_subexpr.sol` fixture
has identical results in named and slot storage modes:

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Approval bytes | 1,110 | 1,140 | +30 (+2.70%) |
| Static TEAL instructions | 656 | 680 | +24 |
| Clear bytes | 4 | 4 | 0 |

The emitted TEAL diff adds exactly six guards and no other instructions:

```text
frame_dig <divisor>
bytec_1                 // numeric zero
b!=
assert                  // zero-divisor diagnostic
```

Each guard is five assembled bytes and four static instructions in this
fixture. The 24-instruction total is spread over six functions, not the
executed cost of every call. Guards proven redundant may optimize away.

Complete compiler, source, corpus and backend identities are in the
[named report](6d940b1e43-dce-named.json) and
[slot report](6d940b1e43-dce-slot.json). These are size measurements, not a new
runtime-validation run. The passing DCE regressions are part of the
[recorded full semantic suite](../../solidity-semantic-tests/README.md).
