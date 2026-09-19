# Reference correctness and boundary reductions

Baseline: `26aadc01e476346d4f2019da39bc431896f86e09`. Candidate: the validated
`rev-2` checkpoint. Compiler SHA-256:
`b9f7e456f537644f47c592e93d7f75615702297ce380113ef517a93829a774fc`.
Production-source identity:
`c3c2def9dff460c8134dadfc19c8d0c21247309717bd147b49807665225fc3e2`.
The backend remains Puya 5.10.1 with unchanged census profiles. The memory
experiment is separate and unchanged.

This checkpoint preserves memory-reference identity, raw-return frames and
recursive memory layout while avoiding unnecessary fresh-value materialization.
It adds calldata-reference return transport, fixes deferred child initialization,
records helper scratch demand per host/reachable free callable, preserves native
EVM return values until wire encoding, and shares scalar-array head validation.

## Unchanged-input size changes

Only unchanged fixture sources and programs successfully compiled on both sides
contribute to the totals. Ten new fixtures and the changed `shared_return_plan.sol`
are excluded. No other regression fixture imports that changed source. Static
TEAL instruction counts are not runtime opcode-budget measurements.

| Corpus | Comparable programs | Approval bytes before | After | Delta | Static instruction delta |
|---|---:|---:|---:|---:|---:|
| Regression, named | 540 | 694,571 | 696,427 | +1,856 | +1,618 |
| Regression, slot | 585 | 792,040 | 790,772 | -1,268 | -579 |
| Tracked chainwide | 6 | 27,585 | 26,659 | -926 | -801 |

Named-mode approval bytes grow **0.27%** overall: 98 programs grow and 35 shrink,
with 6,218 added bytes and 4,362 bytes of reductions. For example,
`MemoryLayoutChecks` shrinks from 6,580 to 5,756 bytes and the large `mstore8`
fixture from 2,599 to 1,874. `DeferredCtorPush` grows from 1,111 to 1,774 bytes;
the corrected initialization path must actually emit its constructor work.
`BoolArrTuple` grows from 3,836 to 4,290. The results do not establish uniform
bytecode reduction.

Slot-mode approval bytes shrink **0.16%** overall: 57 programs grow and 36
shrink, with 3,285 bytes of growth and 4,553 bytes of reductions. All six
comparable tracked-chainwide programs shrink, for a **3.36%** total reduction.
`Eul` drops from 8,534 to 8,231 bytes, `StorageShapes` from 2,908 to 2,693,
and `VANRY` from 6,152 to 5,972. Existing-program clear sizes are unchanged.

Both regression profiles contain 414 Solidity inputs. Named mode emits 560
programs and rejects 44 inputs; slot mode emits 605 and rejects nine. The four
tracked chainwide inputs emit six programs; POL still rejects its unchanged
named profile because raw array/string storage access requires explicit
`--evm-storage-layout`. There are no existing-input compilation transitions,
removed inputs, timeouts or missing-artifact errors in these censuses. Rejected
negative/profile-specific inputs are not runtime semantic failures and do not
contribute fictitious size savings. All three integrity-checked cached repeats
match their canonical tables.

Full deltas, excluded fixtures, compilation outcomes and exact input/compiler/
backend identities: [named](reference-boundaries-regression.json),
[slot](reference-boundaries-regression-slot.json),
[tracked chainwide](reference-boundaries-chainwide.json).

## The two historical large workloads

These are separate from the four-input tracked chainwide census. The same cached
multifile sources and slot-storage/EVM-ABI/xchain profiles were reused. Backend
O2, debug level 1 and AVM 12 options match the historical measurements.

| Contract | Original historical approval | Later reduction check | Current approval |
|---|---:|---:|---:|
| FireBridge | 24,142 | 17,630 | Frontend failure; no bytecode |
| PrivacyPoolSimple | 31,660 | 31,140 | 31,204 |

Clear programs are four bytes for the successful measurements. The historical
figures span several intervening changes: they do not isolate this checkpoint's
effect. PrivacyPool is 456 bytes smaller than the original approximately-32-KB
measurement, but 64 bytes larger than the subsequent reduction check. Its
31,208 combined bytes exceed the replay's 16,384-byte cap by **14,824 bytes**.

FireBridge fails with `Virtual function _update not found`. A small reproducer
and GDB trace identify a foreign-contract virtual-call reachability error in
`BodyFactsWalker::transferCallFacts`: an external callee's body is considered
reachable in its caller's host context. Official solc 0.8.34 compiles the
reproducer in optimized legacy and via-IR modes. This is documented in
[known issues](../../../docs/KNOWN_ISSUES.md), not fixed or hidden by a source
adapter. No new current FireBridge size is claimed.

Fresh historical replay attempts on isolated canonical evaluator ledgers:

- **FBTC:** EVM executes 986/1,000 calls, with 14 closed-world exclusions. AVM
  cannot compile its FireBridge dependency. No paired comparison completes.
- **PrivacyPool:** all 270 EVM calls are excluded because isolated execution
  reverts where historical execution succeeded. The AVM lane independently
  rejects the oversized program. No paired comparison or proof-coverage claim.

No protocol limit was overridden, dependency stubbed, answer tape supplied,
ledger reset or live-chain transaction submitted. The
[workload report](reference-boundaries-real-workloads.json) retains measurements,
historical program hashes, flags, source/harness identities, the minimal
reproducer, solc results, replay outcomes and raw-report hashes. Raw compiler
outputs stay local and ignored.

## Validation and source cost

The complete semantic/framework repeat passes **3,013 tests**, with **zero
failures, 99 xfails and 40 xpasses**, in 1,454.11 seconds. All 3,054 committed
baseline outcomes are unchanged and all 98 additions pass, after normalizing
eight intentional calldata-test renames. Native tests pass **24/24**; focused
validation passes 216 tests with ten xfails. Scalar/ABI differential probes
match **280/280**, with no executed-budget increase among the 208 successful
probes. This does not certify the blocked historical workloads above.

`src/` contains **61,713 physical / 48,130 code lines** in 318 files: **+463
physical / +500 code lines** for the complete checkpoint. The five-item follow-up
alone adds 241 physical / 230 code lines. Counts exclude tests and reports;
code counts also exclude comments and blanks. See the
[semantic baseline](../../solidity-semantic-tests/README.md) for full evidence.

Measurements use one census worker, temporary per-input compilation outputs and
bounded small census caches. Only canonical tables and thin reports/manifests
are versioned. No semantic cache was wiped and no memory-model change was made.
Cached check logs: [named](reference-boundaries-regression-check.log),
[slot](reference-boundaries-regression-slot-check.log),
[chainwide](reference-boundaries-chainwide-check.log).
