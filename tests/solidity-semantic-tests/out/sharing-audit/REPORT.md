# Experimental sharing audit after the `rev-2` rebase

This is context-audit item 4, kept on `experiment/scratch-memory-model`.
Items 1–3 and 5–9 remain on `rev-2`, whose completed source/results tips are
`11803b9778` and `18804ec53f`. No experimental memory implementation was
brought into `rev-2`.

The old experiment tip `453295382a6b1c688ad0964cb2589df59b3858c2` is preserved
on local branch `backup/scratch-memory-before-context-rebase-20260915`.
Rebasing replayed the two prototype commits as `0c2f800cd1` and `ceb72e6ae6`.
The sole conflict kept both the new lowering-result API and the experiment's
memory-reference request scope; it did not change the memory design.

## Implementation

- Join solc-resolved call edges from all applicable host contracts into one
  declaration-keyed sharing fixed point. Late union of independently solved
  hosts could change a callee's convention without updating another caller.
- Track implicit named returns, all return branches, results forwarded as
  arguments or stored into members, and reference children of constructors.
  Unknown internal targets conservatively promote aggregate signatures.
- Scan modifier argument expressions and modifier bodies, including invocations
  without an argument list. Reuse solc-resolved modifiers and existing mutation
  summaries rather than adding another independent mutation analysis.
- Run this analysis only for the scratch profile, after native proxy roots have
  been added. Remove the catch-all exception that hid missing analysis facts.
- Use solc's prepared Yul memory effects to make raw-memory-visible objects
  resident in the arena, including objects not named by the assembly. Scalar-only
  assembly does not trigger blanket promotion. Remove address-only reservations.
- Construct pointer-backed structs using solc's memory size and member offsets,
  retaining existing child pointers. Respect the different legacy/via-IR
  allocation and field-write order checked by the independent EVM oracle.

The fix has **36 fewer physical lines in `src/`** than the rebased selective
checkpoint, including the new constructor handling. These counts include
comments and blank lines; new regression fixtures are outside `src/`.

## Compiler identities

Before: `856af4ddc032980d1f1a37f6f10d65d567a617f7e64a400bdbcfb2feec98a5ff`.

After: `e4487f0063b86eecc18b815dadab8b7381a65164ab18fa32c51e328a27a1830e`.

The after binary is held fixed for all recorded checks. Its build was made from
the working-tree fix on `ceb72e6ae6`, committed after validation as `382c4e0146`.
Dependencies remain Solidity
`a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d` and Puya
`27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1).

## Completed focused and frontend checks

| Check | Result |
|---|---|
| Native CTest | 24/24 passed, 3.80 seconds |
| New sharing checks before the fix | 16 failed, 5 passed, 15.39 seconds |
| New sharing checks after the fix | All 21 passed |
| Combined sharing, existing prototype and context/reference checks | 34 passed, 1 existing mixed-model xfail, 27.95 seconds |
| Independent solc/PyEVM oracle | 28/28 expected results, legacy and via-IR |
| Memory categories, mixed | 128 passed, 5 xfailed, 1 xpassed, 38.78 seconds |
| Memory categories, scratch | 128 passed, 5 xfailed, 1 xpassed, 46.09 seconds |
| Default frontend corpus | 1,770 sources; 3,540 cases; 3,285 successful, 255 failed; no timeouts, 350.580 seconds |
| Default frontend comparison with `rev-2` | All 3,526 common cases have identical AWST, options and exit codes; 14 added successful cases; no removed/changed sources |
| Scratch frontend before/after comparison | 1,770 sources in both storage modes per binary; all 3,540 exit codes unchanged; 3,285 successful and 255 failed per binary; no timeouts; 640.572 seconds combined |

The scratch comparison changes AWST in 88 cases (44 sources in both storage
modes), with no changed options hashes. These are scratch-profile sharing and
constructor changes, not changes to the default profile. Both full scratch
manifests and the list of changed artifact hashes are retained. The corpus
uses three rolling temporary directories and deletes raw output after hashing
each case; it does not retain another complete set of raw AWST files.

The 20 new LocalNet configurations exercise five fixtures under legacy/via-IR
sequencing and ARC-4/EVM ABI. They execute 56 calls with EVM-oracled expectations.
One frontend check also verifies that a unique read-only array parameter stays
value-backed, despite scalar-only assembly. The combined run retains all
sixteen earlier scratch identity probes and twelve context/reference cases.

The oracle uses the local solc binary reporting
`0.8.35-develop.2026.9.3+commit.a99b6d8c.mod.Linux.g++`, optimizer enabled,
Cancun EVM and in-process PyEVM. Its hash is
`46097c69f7428f5866bf693838d8968100ea67e7cb7575879a61a07788e996ca`.
The observable constructor-order expectation is `(71, 71, 128)` for legacy
and `(71, 0, 128)` for via-IR; this difference is deliberate solc behavior.

## Full validation

The full default-profile semantic and framework suite completed with **2,539
passed, 1 failed, 101 xfailed and 39 xpassed** (2,680 total) in **522.16 seconds**.
All 2,657 cases shared with the completed `rev-2` run retained their JUnit
outcomes; none were removed. The added cases are the 21 passing sharing checks,
the original passing scratch identity test and its existing mixed-model xfail.
The only failure remains
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(uint256)(0)` returns zero
instead of reverting because of the pinned Puya backend's DCE bug. This is not
an accepted divergence, and it remains a normal failing test. No new xfail
markers were added.

Command, from the semantic-suite directory, with JUnit reporting enabled:

```text
PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -p lean_validation -q -n 3 --tb=short
```

The validation-only `lean_validation` plugin was supplied on `PYTHONPATH` from
the temporary audit directory. It only removes duplicate harness outputs after
successful teardown, as described below; omit `-p lean_validation` to reproduce
the test checks using the standard harness retention policy. The compiler hash
remained unchanged after the complete run. The independent scratch frontend
corpus overlapped part of the run; this elapsed time is not a standalone
performance benchmark.

This full run uses the default profile except where tests explicitly select
scratch. Scratch validation additionally includes the focused runtime checks,
the 134-case memory-category sweep and the full frontend comparison; it is not
a claim that all semantic tests pass when scratch is forced globally.

## Scope and retained limitations

This validates the sharing fix, not a production-ready unified memory model.
The current memory-category sweep no longer triggers the five ordinary failures
recorded for the earlier unconditional-pointer prototype. Keeping unique objects
value-backed avoids those paths; it does not establish support for pointer-backed
recursive carriers or arbitrary external function pointers in memory words.

Other limitations remain: memory-aggregate modifier parameter transport, tuple
call-return identity, value-backed `bytes`/`string` aliases, the EVM entry router's
aggregate spill boundary, and reference-member deletion. Raw Yul equivalence is
not complete: temporary/conversion allocation order and arbitrary unaligned
pointer rebindings need further work. See
[`docs/scratch-memory-model-prototype.md`](../../../../docs/scratch-memory-model-prototype.md).
No new expected-failure markers were added to hide these gaps.

The initial extra scratch corpus was discarded after detecting a harness setup
error: copying the baseline executable into `/tmp` removed its executable-relative
stdlib search path. The corrected run uses two separately named binaries in the
same ignored build directory, with matching bundled libraries and source hashes.
No compiler code or semantic-test cache was changed for that correction.

Before the final semantic run, a read-only scan checked 127,422 text artifacts
across 28,433 cache entries and checked 3,954 bytecode hashes. It identified 29
damaged entries containing 114 invalid text files after the earlier environment
interruption. Those exact entries were moved to a recoverable local quarantine
(39 MiB), leaving the healthy cache intact. The LocalNet recovery smoke checks
passed 10/10 in 6.69 seconds. The final suite uses the same compiler; no rebuild
or external cache maintenance occurs while tests are running.

The earlier approved storage cleanup removed old raw artifacts; reports and
manifests were preserved. This validation's frontend corpus discards each case's
raw files after hashing. A validation-only pytest teardown plugin also removes
the harness output directory after both the test call and teardown pass;
failed/skipped cases retain their diagnostics. It does not change compiler
inputs, test outcomes, or cache policy. Retained raw diagnostics and the cache
quarantine remain local and ignored; only reports, JUnit files and hash manifests
are retained here. The three top-level research notes remain untracked.
No LocalNet reset was performed.

After every validation process had exited, a one-time oldest-first prune removed
1,729 regenerable source-cache entries (3.33 GiB), returning the cache to
**19.97 GiB**. All 5,066 newest retained source entries and the entire 1.72 GiB
backend cache remain. The exact removal plan and before/after summary are
recorded in `cache-prune-plan.json` and `cache-prune-summary.json`; reports and
the corruption quarantine were outside the deletion targets. The source-cache
deletions are permanent, but their contents can be regenerated from the inputs.
This is not an automatic retention limit added to the test framework.

Across the smoke and final semantic runs, passed-output cleanup removed 2,535
duplicate harness output directories. The suite's complete `out/` tree was about
203 MiB after validation, including retained diagnostics and the quarantine.

The copied pre-fix `before.txt` and `before.xml` normalize trailing whitespace
in pytest failure text. Their original logs remain local; recorded test outcomes
and AWST/options hash manifests are unchanged.
