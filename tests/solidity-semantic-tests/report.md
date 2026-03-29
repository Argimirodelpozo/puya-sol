# Solidity Semantic Tests Report

**Date:** 2026-03-29
**Compiler:** puya-sol @ b7ff11b1
**Total tests:** 1322

## Summary

| Status | Count | % of total |
|---|---|---|
| **Passed** | 473 | 35.8% |
| **Failed** | 353 | 26.7% |
| **Skipped** | 496 | 37.5% |

### Assertions

| Metric | Value |
|---|---|
| Total runnable tests | 1239 |
| Total assertions | 4115 |
| Avg assertions/test | 3.3 |
| Estimated passed assertions | ~1570 / 4115 = ~38.2% |

## By Category

| Category | Pass | Fail | Skip | Total | Pass% |
|---|---|---|---|---|---|
| accessor | 2 | 0 | 0 | 2 | 100% |
| multiSource | 15 | 0 | 0 | 15 | 100% |
| virtualFunctions | 6 | 0 | 0 | 6 | 100% |
| errors | 26 | 1 | 1 | 28 | 93% |
| using | 12 | 1 | 2 | 15 | 80% |
| constants | 8 | 3 | 0 | 11 | 73% |
| literals | 8 | 3 | 0 | 11 | 73% |
| arithmetics | 9 | 2 | 2 | 13 | 69% |
| expressions | 13 | 2 | 4 | 19 | 68% |
| events | 27 | 12 | 5 | 44 | 61% |
| externalSource | 6 | 0 | 4 | 10 | 60% |
| types | 18 | 2 | 12 | 32 | 56% |
| freeFunctions | 5 | 3 | 1 | 9 | 56% |
| state | 11 | 8 | 3 | 22 | 50% |
| modifiers | 15 | 11 | 5 | 31 | 48% |
| viaYul | 30 | 24 | 10 | 64 | 47% |
| inheritance | 17 | 10 | 11 | 38 | 45% |
| variables | 9 | 11 | 1 | 21 | 43% |
| userDefinedValueType | 12 | 12 | 6 | 30 | 40% |
| libraries | 24 | 15 | 23 | 62 | 39% |
| various | 24 | 16 | 28 | 68 | 35% |
| functionCall | 17 | 9 | 22 | 48 | 35% |
| constructor | 8 | 2 | 14 | 24 | 33% |
| functionTypes | 10 | 4 | 17 | 31 | 32% |
| inlineAssembly | 23 | 36 | 19 | 78 | 29% |
| builtinFunctions | 11 | 8 | 19 | 38 | 29% |
| storage | 12 | 8 | 24 | 44 | 27% |
| structs | 16 | 9 | 26 | 51 | 31% |
| array | 10 | 27 | 36 | 73 | 14% |
| abiEncoderV1 | 3 | 17 | 8 | 28 | 11% |
| abiEncoderV2 | 1 | 17 | 26 | 44 | 2% |
| abiEncodeDecode | 0 | 6 | 13 | 19 | 0% |
| tryCatch | 0 | 0 | 20 | 20 | 0% |
| fallback | 0 | 9 | 2 | 11 | 0% |

## Session Fixes (2026-03-29)

- **addmod/mulmod mod-by-zero revert** (+1 test) — assert(z != 0) before biguint mod
- **unchecked uint64 wrapping** (+2 tests) — mask result % 2^bits for sub-64-bit types
- **sol-eb/ wired into build** — 16 source files were never compiled! Now linked and BuiltinCallableRegistry dispatches mulmod/addmod/keccak256/sha256/gasleft
- **Removed FunctionCallBuilder bloat** — deleted duplicate inline handlers (-110 lines)
- **Box-stored dynamic arrays** — .push() via box_resize, .length via box_len/elemSize, __postInit box_create
- **Removed abi.decode blanket skip** — tests now run (fail with specific errors vs hidden)

## Top Blockers

1. **Box array slot ops** (~61 tests) — puya backend uses scratch slots for ReferenceArray. Partially fixed with box_resize/box_len approach but needs __postInit deployment support
2. **Extract OOB** (~54 tests) — wrong byte offsets, often from box array slot issue
3. **Budget exceeded** (~12 tests) — biguint ops 20x more expensive than uint64. Needs Yul uint64 promotion
4. **2D/3D arrays** (~25 tests) — new T[][]() not implemented
5. **Function pointers** (~22 tests) — dispatch table not implemented
6. **EVM ABI return format** — test assertions use EVM offset/length encoding, ARC4 differs
