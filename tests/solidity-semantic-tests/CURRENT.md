# Semantic Test Status — v142

**Totals**: 1009 PASS / 247 FAIL / 66 COMPILE_ERR = **1009/1322 (76.3%)**

vs v141 (1008): `array/storage_array_ref` gained (COMPILE_ERR → PASS via `blockAlwaysTerminates` / `removeDeadCode` recursion through nested Blocks — needed for brace-less if/else branches wrapping overflow-check + return). `uncalled_blockhash` flakes under load (block-seed-from-future-round race); passes isolated. `mapping_enum_key_v1` still flaky same class.

## Category breakdown (sorted by gap size)

| Category | PASS | FAIL | COMPILE_ERR | Total | Pass % |
|---|---:|---:|---:|---:|---:|
| array | 44 | 27 | 2 | 73 | 60.3% |
| inlineAssembly | 50 | 24 | 4 | 78 | 64.1% |
| various | 46 | 11 | 11 | 68 | 67.6% |
| abiEncoderV2 | 24 | 20 | 0 | 44 | 54.5% |
| libraries | 42 | 13 | 7 | 62 | 67.7% |
| tryCatch | 0 | 19 | 1 | 20 | 0.0% |
| functionCall | 29 | 14 | 5 | 48 | 60.4% |
| storage | 28 | 9 | 7 | 44 | 63.6% |
| storageLayoutSpecifier | 22 | 11 | 1 | 34 | 64.7% |
| abiEncoderV1 | 18 | 10 | 0 | 28 | 64.3% |
| userDefinedValueType | 21 | 9 | 0 | 30 | 70.0% |
| viaYul | 55 | 8 | 1 | 64 | 85.9% |
| structs | 43 | 5 | 3 | 51 | 84.3% |
| abiEncodeDecode | 12 | 6 | 1 | 19 | 63.2% |
| builtinFunctions | 31 | 6 | 1 | 38 | 81.6% |
| externalContracts | 1 | 3 | 4 | 8 | 12.5% |
| constructor | 18 | 6 | 0 | 24 | 75.0% |
| modifiers | 25 | 4 | 2 | 31 | 80.6% |
| getters | 9 | 2 | 3 | 14 | 64.3% |
| calldata | 20 | 4 | 0 | 24 | 83.3% |
| immutable | 14 | 2 | 2 | 18 | 77.8% |
| inheritance | 34 | 3 | 1 | 38 | 89.5% |
| memoryManagement | 1 | 4 | 0 | 5 | 20.0% |
| cleanup | 16 | 3 | 0 | 19 | 84.2% |
| isoltestTesting | 8 | 1 | 2 | 11 | 72.7% |
| saltedCreate | 0 | 3 | 0 | 3 | 0.0% |
| state | 19 | 2 | 1 | 22 | 86.4% |
| using | 12 | 1 | 2 | 15 | 80.0% |
| variables | 18 | 3 | 0 | 21 | 85.7% |
| errors | 26 | 1 | 1 | 28 | 92.9% |
| events | 42 | 2 | 0 | 44 | 95.5% |
| experimental | 0 | 0 | 2 | 2 | 0.0% |
| fallback | 9 | 2 | 0 | 11 | 81.8% |
| revertStrings | 22 | 2 | 0 | 24 | 91.7% |
| types | 30 | 2 | 0 | 32 | 93.8% |
| constantEvaluator | 1 | 1 | 0 | 2 | 50.0% |
| deployedCodeExclusion | 11 | 0 | 1 | 12 | 91.7% |
| externalSource | 9 | 0 | 1 | 10 | 90.0% |
| reverts | 9 | 1 | 0 | 10 | 90.0% |
| shanghai | 1 | 1 | 0 | 2 | 50.0% |
| smoke | 9 | 1 | 0 | 10 | 90.0% |
| uninitializedFunctionPointer | 5 | 1 | 0 | 6 | 83.3% |

### 100% passing (26 categories, 175 tests)

| Category | Tests |
|---|---:|
| accessor | 2 |
| arithmetics | 13 |
| constants | 11 |
| conversions | 2 |
| ecrecover | 5 |
| enums | 11 |
| exponentiation | 3 |
| expressions | 19 |
| freeFunctions | 9 |
| functionSelector | 1 |
| functionTypes | 31 |
| integer | 5 |
| interfaceID | 6 |
| literals | 11 |
| metaTypes | 1 |
| multiSource | 15 |
| operators | 3 |
| optimizer | 2 |
| payable | 1 |
| receive | 3 |
| scoping | 1 |
| specialFunctions | 3 |
| statements | 2 |
| strings | 8 |
| underscore | 1 |
| virtualFunctions | 6 |

## Top gap categories (by absolute fails+compile errors)

1. **array** (29) — dynamic array encoding edge cases
2. **inlineAssembly** (28) — Yul gaps (keccak256 non-constant offsets, deep recursion)
3. **various** (22) — mixed puya-backend crashes
4. **abiEncoderV2** (20) — EVM-byte-identity encode comparisons
5. **libraries** (20) — library-as-contract + function-pointer patterns
6. **tryCatch** (20) — AVM-incompatible (no analogue for revert bubbling)
7. **functionCall** (19) — cross-contract patterns
8. **storage** (16) — storage boundaries + sign-bit chopping
