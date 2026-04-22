# Semantic Test Status — v141

**Totals**: 1008 PASS / 247 FAIL / 45 COMPILE_ERR / 21 DEPLOY_ERR = **1008/1322 (76.2%)**

vs v140 (1008): `freeFunctions/import` gained (FAIL → PASS via free-function storage-ref augmentation); `types/mapping_enum_key_v1` regressed under load but passes isolated (flaky, same class as mapping_enum_key_v2 at v140). Real underlying count is 1009.

## Category breakdown (sorted by gap size)

| Category | PASS | FAIL | COMPILE_ERR | Total | Pass % |
|---|---:|---:|---:|---:|---:|
| array | 43 | 27 | 2 | 73 | 58.9% |
| inlineAssembly | 50 | 24 | 4 | 78 | 64.1% |
| various | 46 | 11 | 3 | 68 | 67.6% |
| abiEncoderV2 | 24 | 20 | 0 | 44 | 54.5% |
| libraries | 42 | 13 | 7 | 62 | 67.7% |
| tryCatch | 0 | 19 | 1 | 20 | 0.0% |
| functionCall | 29 | 14 | 3 | 48 | 60.4% |
| storage | 28 | 9 | 6 | 44 | 63.6% |
| storageLayoutSpecifier | 22 | 11 | 1 | 34 | 64.7% |
| abiEncoderV1 | 18 | 10 | 0 | 28 | 64.3% |
| userDefinedValueType | 21 | 9 | 0 | 30 | 70.0% |
| viaYul | 55 | 8 | 1 | 64 | 85.9% |
| structs | 43 | 5 | 3 | 51 | 84.3% |
| abiEncodeDecode | 12 | 6 | 0 | 19 | 63.2% |
| builtinFunctions | 31 | 6 | 1 | 38 | 81.6% |
| externalContracts | 1 | 3 | 2 | 8 | 12.5% |
| constructor | 18 | 6 | 0 | 24 | 75.0% |
| modifiers | 25 | 4 | 2 | 31 | 80.6% |
| getters | 9 | 2 | 2 | 14 | 64.3% |
| calldata | 20 | 4 | 0 | 24 | 83.3% |
| inheritance | 34 | 3 | 0 | 38 | 89.5% |
| memoryManagement | 1 | 4 | 0 | 5 | 20.0% |
| cleanup | 16 | 3 | 0 | 19 | 84.2% |
| immutable | 14 | 2 | 0 | 18 | 77.8% |
| isoltestTesting | 8 | 1 | 0 | 11 | 72.7% |
| saltedCreate | 0 | 3 | 0 | 3 | 0.0% |
| types | 29 | 3 | 0 | 32 | 90.6% |
| using | 12 | 1 | 2 | 15 | 80.0% |
| variables | 18 | 3 | 0 | 21 | 85.7% |
| errors | 26 | 1 | 1 | 28 | 92.9% |
| events | 42 | 2 | 0 | 44 | 95.5% |
| experimental | 0 | 0 | 2 | 2 | 0.0% |
| fallback | 9 | 2 | 0 | 11 | 81.8% |
| revertStrings | 22 | 2 | 0 | 24 | 91.7% |
| state | 20 | 1 | 0 | 22 | 90.9% |
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
3. **abiEncoderV2** (20) — EVM-byte-identity encode comparisons
4. **libraries** (20) — library-as-contract + function-pointer patterns
5. **tryCatch** (20) — AVM-incompatible (no analogue for revert bubbling)
6. **functionCall** (17) — cross-contract patterns
7. **various** (14) — mixed puya-backend crashes
