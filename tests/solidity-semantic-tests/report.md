# Solidity Semantic Tests Report

**Date:** 2026-03-29
**Compiler:** puya-sol @ c5c22b20 + xfail removal (070093f0)
**Total tests:** 1322

## Summary

| Status | Count | % of total |
|---|---|---|
| **Passed** | 470 | 35.6% |
| **Failed** | 347 | 26.2% |
| **Skipped** | 505 | 38.2% |

## Skipped Breakdown (505)

| Reason | Count | % of total |
|---|---|---|
| Compilation failed | ~250 | 18.9% |
| No deployable contracts | ~100 | 7.6% |
| Deploy failed | ~13 | 1.0% |
| Parser skip: ABIEncoderV1 | ~26 | 2.0% |
| Parser skip: delegatecall | ~13 | 1.0% |
| Parser skip: try/catch | ~20 | 1.5% |
| Parser skip: abi.decode | ~25 | 1.9% |
| Parser skip: receive/fallback | ~8 | 0.6% |
| Parser skip: storage layout | ~34 | 2.6% |
| Parser skip: other | ~16 | 1.2% |

## Failed Breakdown (347)

### By root cause

| Root cause | Est. count | Fixable? |
|---|---|---|
| Wrong return values (got 0/None) | ~93 | Case-by-case compiler bugs |
| Value mismatch (got wrong number) | ~78 | Case-by-case compiler bugs |
| Runtime crash (assert/extract/loads) | ~100 | Various — biggest is `loads arg 0` (slot arrays) |
| Missing revert (expected FAILURE) | ~32 | EVM-specific validation not implemented |
| True->False | ~20 | Comparison/logic bugs |
| Framework errors | ~24 | Python test harness issues |

### Top runtime crash types

| Crash | Count | Root cause |
|---|---|---|
| `loads arg 0` | ~61 | Box-stored dynamic arrays use slot ops — puya backend bug |
| `extraction start/end` | ~54 | extract3/substring3 OOB — wrong offset/length |
| `assert failed` | ~26 | Logic errors in generated TEAL |
| `math attempted` | ~23 | Division by zero / overflow |
| `budget exceeded` | ~12 | Biguint ops too expensive (needs uint64 optimization) |

## By Category

| Category | Pass | Fail | Skip | Total | Pass % |
|---|---|---|---|---|---|
| inlineAssembly | 23 | 37 | 18 | 78 | 29.5% |
| array | 11 | 34 | 28 | 73 | 15.1% |
| various | 40 | 28 | 0 | 68 | 58.8% |
| viaYul | 37 | 27 | 0 | 64 | 57.8% |
| abiEncoderV2 | 18 | 26 | 0 | 44 | 40.9% |
| libraries | 44 | 18 | 0 | 62 | 71.0% |
| events | 30 | 14 | 0 | 44 | 68.2% |
| inheritance | 22 | 16 | 0 | 38 | 57.9% |
| functionCall | 21 | 14 | 13 | 48 | 43.8% |
| modifiers | 18 | 13 | 0 | 31 | 58.1% |
| variables | 8 | 13 | 0 | 21 | 38.1% |
| userDefinedValueType | 13 | 13 | 4 | 30 | 43.3% |
| storage | 14 | 11 | 19 | 44 | 31.8% |
| structs | 26 | 10 | 15 | 51 | 51.0% |
| tryCatch | 0 | 0 | 20 | 20 | 0.0% |
| state | 11 | 8 | 3 | 22 | 50.0% |
| errors | 26 | 1 | 1 | 28 | 92.9% |

## What Would Move the Needle Most

1. **`loads arg 0` fix** (~61 tests) — puya backend generates wrong slot ops for box-stored dynamic arrays (.push/.pop/.length). Needs puya Python fix, not C++ frontend.

2. **Extract OOB** (~54 tests) — wrong byte offsets in generated TEAL. Scattered compiler bugs across multiple categories.

3. **Yul uint64 promotion** (~12 tests) — Yul loop variables default to biguint (20 budget/op). Proven concept: 100K budget -> 691 budget for simple loop. Needs selective application to avoid regressions.

4. **Parser skip removal** (~142 tests) — some skipped categories may partially work now (fallback, receive, abi.decode). Could recover tests without compiler changes.

5. **2D/3D array creation** (~25 tests) — `new T[][]()` emits `InstanceMethodTarget("unknown")`. Needs NewExpression handler for multi-dimensional arrays.

6. **Function pointers** (~22 tests) — dynamic dispatch table not implemented. Complex feature.
