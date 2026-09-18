# Typed-value reductions and enum-array places

Baseline: `08b016037cae97f45c54ad7cf8ca53032f454efd`. Candidate: the `rev-2`
working tree described by these reports. Compiler SHA-256:
`cedd6e6596c5f2ae8d26a236a76fdd96b17aff36fd6e090c986882f861fa7e7e`.
Production-source identity:
`8ae0de0c4d8a3dc46de93883fd30c8544c38ec0f223ecd0119f0d970cb277345`.
Both measurements use the installed Puya 5.10.1 backend and unchanged size-profile
flags. The memory experiment is untouched.

The pass fixes enum-array writes that targeted validation temporaries instead
of their array elements. It shares full-width enum checks, checks the actual
implicit-return value before encoding, removes value-only builder wrappers and
the obsolete member-access fallback, shares comparison emission, and deletes
unused reverse dispatch and four unused AWST makers. Named-array lengths,
dynamic-element bounds guards and mapping-array push/pop share a bounded
two-byte ARC4 count reader. Raw bytes/string and slot storage retain their
separate length representations; no storage format changes.

## Existing-program deltas

Only successful programs present in both versions contribute to these totals.
The new fixture adds three programs per storage profile; these are excluded
from existing-program growth. Static instruction counts are not runtime cost.

| Corpus | Comparable programs | Approval bytes before | After | Delta | Static instruction delta |
|---|---:|---:|---:|---:|---:|
| Regression, named | 552 | 700,907 | 700,714 | -193 | -240 |
| Regression, slot | 575 | 763,379 | 763,383 | +4 | +4 |
| Tracked chainwide | 8 | 35,470 | 35,470 | 0 | 0 |

All existing compile outcomes and clear-program sizes are unchanged. There are
no compile regressions, recoveries, removed programs, timeouts or artifact errors.
Each regression profile contains 395 Solidity inputs: named mode compiles 555
programs and rejects 29 inputs; slot mode compiles 578 and rejects nine. The
four tracked chainwide inputs compile eight programs. Rejected inputs include
intentional negative and profile-specific fixtures, not runtime test failures.

Named mode has 38 shrinking programs and one growing program:
`ArrayStructMappingAlias` adds 11 bytes and seven static instructions with the
bounded header reader. Slot mode changes only `EnumBoundaryChecks`, adding four
bytes/instructions with the actual-value implicit-return check. No claim of
uniform bytecode reduction is made.

Full deltas and compiler/input/backend identities:
[named](eb-values-regression.json), [slot](eb-values-regression-slot.json),
[chainwide](eb-values-chainwide.json).

## Array-length runtime benchmark

The unchanged `NamedArrayLengths` contract body is compiled before and after
the changes. Its query reads lengths of `uint128[]`, `uint8[]`, `bytes[]` and
mapping-element arrays. Each version is deployed on the existing LocalNet and
queried when empty, after each of nine pushes, after a pop and after deletion.
Every query returns the expected lengths.

| Measurement | Before | After | Delta |
|---|---:|---:|---:|
| Approval program bytes | 1,187 | 1,167 | -20 |
| Executed query opcode-budget units | 142 | 136 | -6 |

The query cost is the same across all 12 measured states in each version.
This supports replacing the stride-based length reconstruction with the shared
header reader, while bounding the amount of data read from dynamic-element boxes.
Evidence: [baseline](../../solidity-semantic-tests/out/eb-values/length-baseline.json),
[candidate](../../solidity-semantic-tests/out/eb-values/length-candidate.json),
[benchmark driver](../../solidity-semantic-tests/out/eb-values/length_benchmark.py).

## Validation and retention

Native tests pass 24/24; the focused LocalNet matrix passes 152/152, including
all 24 new configurations. Official solc 0.8.34 execution confirms 166 checks
in optimized legacy and via-IR modes. The pre-fix runtime regression returns
zero instead of one after a memory enum-array assignment, proving the new test
detects the original bug.

All three canonical size tables pass cached checks. A separate cold chainwide
repeat also has zero differences. The size-reporting tool's 27 unit tests pass.
Named, slot and chainwide censuses took 366.93, 423.08 and 10.69 seconds with
four workers. Only thin reports and canonical tables are retained; temporary
size-compilation outputs are removed by the size tool. No semantic-cache or
ledger reset was performed.

`src/` removes **604 physical / 443 code lines**, leaving **61,877 physical /
48,017 code lines** in 318 files. Tests and reports are not included in these
source counts. The complete semantic/framework repeat passes **2,829 tests**
with **zero failures, 99 xfails and 40 xpasses** in 1,169.03 seconds. All 2,944
previous individual outcomes are unchanged, and all 24 new configurations pass.
Compiler, source and test fingerprints remain unchanged after validation. See
the [suite baseline](../../solidity-semantic-tests/README.md),
[outcome comparison](../../solidity-semantic-tests/out/eb-values/semantic-comparison.json)
and [validation manifest](../../solidity-semantic-tests/out/eb-values/validation-manifest.json).
