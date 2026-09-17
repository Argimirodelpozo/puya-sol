# Lowering and Solidity/Yul boundary size comparison

Baseline: `c65593221fd59fc609414b01e849610cd1e74142`. Candidate: the validated
`rev-2` working tree committed with this report. The candidate compiler SHA-256
is `a5425b3dc191bc78cd36ef77c15355ac74d719f21d70ec7370e0d3168612b685`, and its
production-source identity is
`1da723a9527c6bb8be298a10e978dfc63830d4ae99fa8f69a27684f08964965f`.

Before rows come from the committed tables and their recorded compiler/input
identities; after rows are fresh full-corpus compilations. All 391 existing
regression Solidity inputs, the four tracked chainwide inputs and the size tool
are unchanged. Three new regression fixtures add 25 programs in each storage
profile; they are not counted as growth of existing programs. Both measurements
use the same installed Puya 5.10.1 backend and fixed profile flags.

Regression profiles use legacy sequencing and ARC4 ABI in named/slot storage
modes. Chainwide uses EVM ABI, with slot storage for Eul. These fixed size
profiles differ from each semantic fixture's individual settings: `ERR` rows
include negative and profile-specific inputs, not runtime test failures.

## Current tables

| Corpus | Sources | Compiled programs | Rejected sources |
|---|---:|---:|---:|
| Regression, named | 394 | 552 | 29 |
| Regression, slot | 394 | 575 | 9 |
| Tracked chainwide | 4 | 8 | 0 |

There are no compile regressions, recoveries, removed programs or removed
inputs. Every pre-existing compile outcome is unchanged. No timeout,
missing/empty-artifact error or empty-output success was recorded.

## Existing-program deltas

Only successful programs present on both sides contribute to these totals.
Static TEAL instructions are not executed opcode budgets.

| Corpus | Comparable programs | Approval bytes before | After | Delta | Static TEAL instruction delta |
|---|---:|---:|---:|---:|---:|
| Regression, named | 527 | 683,643 | 685,279 | +1,636 (+0.24%) | +1,196 |
| Regression, slot | 550 | 745,616 | 747,379 | +1,763 (+0.24%) | +1,312 |
| Tracked chainwide | 8 | 35,470 | 35,470 | 0 | 0 |

Clear-program bytes are unchanged throughout. Named mode has 33 growing and
13 shrinking programs; slot mode has 34 growing and 15 shrinking programs.
All eight tracked chainwide programs retain their sizes and instruction counts.

The largest named increase is `NativePaymentSender`: 3,022 to 3,296 bytes
(+274; slot +272). Empty value calls now execute the receiver application and
replace returndata, rather than only sending a payment. Other increases are
concentrated in fixtures exercising immutable calldata and full-word scalar
lifetimes: `AsmCdLayout` +198, `signed_asm_read_word.sol:C` +172 and
`YulScopedFacts` +169 bytes in both profiles. Slot mode's `EnumBoundaryChecks`
adds 139 bytes; the same source is rejected under the fixed named-size profile.
These correctness changes are not claimed to reduce emitted bytecode overall.

Shared lowering also removes code: `ExpressionCalldataFields` shrinks by 118
bytes in both profiles; `lowering_calls.sol:Probe` shrinks by 69 bytes in named
storage and 39 bytes in slot storage. The
complete RIPEMD AWST helper is unchanged, including source locations; its
[identity evidence](../../solidity-semantic-tests/out/lowering-boundaries/ripemd-identity.json)
is separate from whole-program size comparisons.

Full per-program deltas and before/after compiler, input and backend identities:
[named](lowering-boundaries-regression.json),
[slot](lowering-boundaries-regression-slot.json),
[chainwide](lowering-boundaries-chainwide.json).

## Validation and retention

All three canonical tables pass cached `--check`. An independent
`--no-cache --check` chainwide repeat has zero differences in bytes, static
instruction counts or compile outcomes. The size-tool unit tests pass 27/27.

| Measurement/check | Workers | Wall time |
|---|---:|---:|
| Fresh named table | 2 | 617.89 s |
| Fresh slot table | 2 | 676.27 s |
| Fresh chainwide table | 2 | 17.22 s |
| Cached checks, named / slot / chainwide | 2 | 1.91 / 2.26 / 1.95 s |
| Cold chainwide repeat | 2 | 17.12 s |

Check logs: [named](lowering-boundaries-regression-check.log),
[slot](lowering-boundaries-regression-slot-check.log),
[chainwide](lowering-boundaries-chainwide-check.log),
[cold chainwide](lowering-boundaries-chainwide-cold-check.log).

The final [semantic and framework run](../../solidity-semantic-tests/README.md)
has **2,805 passed, zero failed, 99 xfailed and 40 xpassed** in 999.55 seconds.
All 2,887 prior individual outcomes are unchanged and 57 new configurations pass.
Native CTests pass 24/24; the focused matrix passes 217/217. Source and compiler
hashes remained unchanged through both semantic and size validation.

`lowering/` is 193 physical / 133 code lines smaller. The overall correctness
and reduction pass adds 433 physical / 469 code lines to `src/`, now 62,481
physical / 48,460 code lines across 320 files. The memory experiment is separate.

Only thin reports and tables are committed. Size-compilation outputs were
temporary and removed automatically; semantic generated artifacts remain
ignored. No historical compiler build, ledger reset or semantic-cache clearing
was needed for this comparison.
