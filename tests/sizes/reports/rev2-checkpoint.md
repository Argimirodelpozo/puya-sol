# Typed-value/Yul reductions and returned-memory correctness

Baseline: the committed size tables at
`08b016037cae97f45c54ad7cf8ca53032f454efd`. Candidate: the complete `rev-2`
checkpoint, including the typed-value, Yul/storage and solc-fact passes.
Compiler SHA-256:
`eca28dca489d25cc60c73c589398f0c2865fb5d335b97520a3474b9ea98608c0`.
Production-source identity:
`3692366ed6190cfa82014d83e3472f0dc6c8fbdaa07a0b02403bfb12574fd8de`.
The installed backend is Puya 5.10.1; size-profile flags are unchanged.
The memory experiment remains separate.

## Like-for-like size changes

Only unchanged fixture sources and programs successfully compiled on both
sides contribute to this table. New fixtures, moved contracts and changed
fixture sources are excluded; the JSON reports also retain the complete
table comparisons. Static TEAL instruction counts are not runtime opcode cost.

| Corpus | Comparable programs | Approval bytes before | After | Delta | Static instruction delta |
|---|---:|---:|---:|---:|---:|
| Regression, named | 524 | 653,205 | 664,697 | +11,492 | +7,291 |
| Regression, slot | 563 | 737,948 | 749,522 | +11,574 | +7,343 |
| Tracked chainwide | 6 | 25,780 | 27,585 | +1,805 | +1,196 |

Named-mode approval bytes grow **1.76%** across these unchanged programs:
28 grow and 36 shrink. Growth totals 12,150 bytes, versus 658 bytes of
reductions. Existing-program clear sizes are unchanged. This is a source-code
reduction with correctness-related generated-code growth, not a claim of
uniform bytecode reduction.

Slot-mode approval bytes grow **1.57%**, with 27 growing and six shrinking
unchanged programs. The six comparable chainwide programs grow **7.00%**:
`Eul` adds 609 bytes, `ERC20PresetMinterPauser` 599 and `VANRY` 597.
The other three are unchanged. POL is excluded from those size totals because
its original profile now rejects, as explained below; it is not counted as
a bytecode reduction.

Full table deltas, the filtered comparisons and compiler/input/backend identities:
[named](rev2-checkpoint-regression.json),
[slot](rev2-checkpoint-regression-slot.json),
[chainwide](rev2-checkpoint-chainwide.json).

The returned-memory fix introduces pointer-based transport, ABI-boundary
materialization and return-object initialization where required by the internal
carrier. The largest named-mode increases include `ExternalResultFacts`
(3,872 to 5,511 bytes), `AsaOperandOrder` (464 to 1,490) and
`ArrayConversions` (5,568 to 6,585). A useful follow-up is to prove when
value-only memory returns can avoid pointer transport and materialization,
without losing aliases or observable initialization behavior. This checkpoint
does not attempt that optimization.

## Compile outcomes and fixture changes

Eleven previously successful named-mode inputs now reject raw array-storage
access with an explicit `--evm-storage-layout` diagnostic. This is the selected
storage policy, not an automatic backend switch. The JSON report lists each
transition; all eleven diagnostics were checked. Such rejected inputs do not
contribute fictitious size savings.

Fixtures containing slot-only cases were split so their remaining named-mode
coverage can continue. Changed and newly added sources are listed separately
in each regression report. Negative and profile-specific inputs remain visible
as `ERR` rows; this size census is not the semantic test score.

Both regression profiles contain 404 Solidity inputs. Named mode emits 541
programs and rejects 44 inputs; slot mode emits 586 programs and rejects nine.
All existing slot-mode compile outcomes are unchanged. The four tracked
chainwide inputs emit six programs, with POL rejecting in its unchanged
named-storage profile: OpenZeppelin `StorageSlot.getStringSlot` performs raw
string-storage reference rebinding, which requires explicit slot mode.

A separate [POL compile check](rev2-checkpoint-pol-slot.json) with
`--evm-storage-layout` succeeds, emitting `ERC20` at 2,279 bytes and
`PolygonEcosystemToken` at 8,182 bytes. This is a compile-only check, not a new
historical replay. Its layout differs from the old canonical profile, so these
sizes are not presented as like-for-like deltas. The canonical profile is not
silently changed; its `ERR` row records the requirement accurately.

## Validation and retention

The complete semantic/framework repeat passes **2,915 tests**, with **zero
failures, 99 xfails and 40 xpasses**, in 2,552.28 seconds. All 2,968 previous
individual outcomes are unchanged and all 86 added configurations pass.
Native CTests pass **24/24**. The focused repeat passes 182 tests, with 13
xfails and four xpasses. Official solc 0.8.34 legacy/via-IR execution confirms
52 checks for the new returned-memory fixtures, including public-library
copy boundaries. See the [suite baseline](../../solidity-semantic-tests/README.md)
for the reports and input manifests.

The complete checkpoint removes **1,231 physical / 830 code lines** from
`src/`, leaving **61,250 physical / 47,630 code lines** in 318 files. The last
solc sub-pass itself adds 163 physical / 225 code lines; the cumulative
reduction is not attributed to that sub-pass alone.

All three canonical tables pass integrity-checked cached comparisons. There
are no timeouts or missing-artifact errors. Size measurements use one worker.
Per-input compilation outputs are temporary
and removed immediately; only canonical tables, thin reports and bounded
census caches are retained. No ledger or semantic-cache reset was performed.
