# Solidity Semantic Tests Report

**Date:** 2026-03-30
**Compiler:** puya-sol @ ab56f249
**Total tests:** 1322

## Summary

| Status | Count | % of total |
|---|---|---|
| **Passed** | 481 | 36.4% |
| **Failed** | 361 | 27.3% |
| **Skipped** | 480 | 36.3% |

## Session Progress (2026-03-29 → 2026-03-30)

- **Baseline:** 470 passed (with stale cache inflating to ~476)
- **True baseline (fresh compile):** ~462 passed
- **After all fixes:** 481 passed (+19 from true baseline)
- Parser skips removed — all tests compile or fail honestly
- No stale cache — output dirs cleaned before each compile

### Fixes applied
- addmod/mulmod mod-by-zero revert (+1)
- Unchecked uint64 wrapping for sub-64-bit types (+2)
- Enum range validation on type cast (+2)
- Assembly invalid() opcode (+1)
- Transient storage blob model (tload/tstore + Solidity transient vars) (+3)
- sol-eb/ wired into build (16 source files were dead code)
- BuiltinCallableRegistry dispatching (removed duplicate FunctionCallBuilder handlers)
- Left-aligned bytesN arg fix — all interfaceID tests pass (+4)
- Address string → integer comparison in test framework (+1)
- Encoding import fix for _call_with_payment (+5)
- Removed abi.decode blanket skip
- Cache fix — no stale artifacts
