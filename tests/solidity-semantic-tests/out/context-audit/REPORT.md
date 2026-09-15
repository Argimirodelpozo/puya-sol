# Context audit: completed `rev-2` verification

Status on 2026-09-15: the `rev-2` implementation is committed as `11803b9778`,
based on merge checkpoint `8a44a9ddda304daae98f1f81af7fbe0c40c9360d`.
The completed full semantic repeat is the recorded baseline in the suite README.
All 2,647 preceding cases retained their outcomes, all ten new cases passed,
and no case was removed. The one known backend failure remains open.

The experimental checkpoint `453295382a` was committed and pushed to
`origin/experiment/scratch-memory-model`. Item 4 remains on that separate work
stream; neither its fix nor the requested rebase onto completed `rev-2` has
been performed yet. No experimental memory-model code was brought into `rev-2`.

## Implementation

| Audit item | `rev-2` changes |
|---|---|
| 1 | Storage-reference offset propagation resolves the actual internal-call implementation in each reachable solc host and transfers arguments by formal position. Parenthesized and conditional sources are followed. |
| 2 | Literal tuple bindings and assignments contribute memory-reference alias edges. Tuple lowering preserves existing memory pointers, including holes, parenthesized identifiers, swaps and conditional components. |
| 3 | `CompilationSession` explicitly disallows copying and moving, keeping the references held by its mapper valid. |
| 4 | Experimental sharing analysis: pending on the experiment branch, not included here. |
| 5 | Function symbols are derived directly from solc declaration identity. The mutable symbol table and registration pass are removed; runtime function-pointer IDs remain. |
| 6 | Existing contract/function indexes are reused for creation/body scans; reference transfer handling is consolidated. |
| 7 | Expression lowering has one value/effects result API; the unused duplicate source-type field is removed. |
| 8 | Scoped translation state reuses solc's `ScopedSaveAndRestore`, including exception-safe restoration. |
| 9 | Yul signature metadata is obtained from the source declaration and boundary plan on demand; referenced integer widths come from solc's assembly external references. |

The regression also exposed parenthesized storage-argument bugs in both
storage modes. Slot lowering treated the expression as a materialized value
rather than resolving its slot; the frozen pre-fix compiler reproduced this
rejection. Named storage lost the array-element offset through parenthesized
arguments and indexed bases, generating writes to the wrong box. Both paths
now unwrap expressions using the existing solc fact helper. The final probes
include `touch((item))` and `relay(((items)[1]))` in inherited implementations.

These changes do not implement general memory-call-return alias provenance or
the experimental sharing-analysis fixes. The existing mixed representation
remains in use.

The source delta against the merge checkpoint is **94 fewer physical lines in
`src/`**, including the new 29-line declaration-identity header, and **165 fewer
in `src/builder/context/`**. These counts include comments and blank lines.

## Completed checks

Compiler SHA-256:
`f53ceaf6355a0a924f5750d78345c964fb69c5ed155bd16c7e10b20ba4c0e1de`.
The compiler remained unchanged throughout the final checks.

| Check | Result |
|---|---|
| Native CTest | 24/24 passed, 4.45 seconds |
| Full LocalNet semantic and framework suite | 2,517 passed; 1 failed; 100 xfailed; 39 xpassed; 391.74 seconds |
| Comparison with the preceding semantic baseline | 2,647 unchanged outcomes; ten added passes; zero removals |
| Focused LocalNet regressions | 13/13 passed, 17.24 seconds |
| LocalNet recovery smoke test | 8/8 passed, 10.00 seconds, no ledger reset |
| Independent solc/PyEVM regression oracle | 22/22 expected results, legacy and via-IR |
| Full frontend corpus, both storage modes | 1,763 sources; 3,526 cases; 3,271 successes; 255 failures; zero timeouts |
| Comparison with the preceding layout corpus | All 3,524 existing exit codes unchanged; no sources changed or cases removed; two new successful cases |

The focused runtime checks cover eight new regression configurations
(legacy/via-IR sequencing, ARC-4/EVM ABI, and named/slot storage), four existing
reference-boundary identity configurations, and the existing destructuring
assignment fixture. All execute on LocalNet, without ledger reset.

Earlier offline checks on the preceding working-tree compiler passed 15/15
framework tests and both eight-configuration backend matrices (24 new-fixture
artifacts and eight destructuring artifacts). Their historical `framework.*`
and `backend-*-summary.json` reports are retained, but do not identify the final
binary above. The initial persistent Puya worker stalled; those offline backend
matrices completed using `PUYA_SOL_NO_SERVE=1`. Final LocalNet validation uses
the normal harness and persistent backend.

The independent EVM oracle used the local solc binary reporting
`0.8.35-develop.2026.9.3+commit.a99b6d8c.mod.Linux.g++`, optimizer enabled,
Cancun EVM, and in-process PyEVM. Its exact version, hash and per-call results
are in `solc-oracle.json`; the `.mod` version suffix is retained explicitly.

The raw frontend comparison records 55 changed AWST files and no changed
options files among existing cases. Of these, 53 differ solely by the
`__evmmem_off_` to `__evmmem_boff_` bytes-allocation name fix already committed
on `rev-2` as `5573dec612`, which the older layout baseline did not contain.
The remaining two are `various/contracts/destructuring_assignment.sol` in
named and slot storage: its tuple binding now carries memory alias identity.
This is a functional fix, not the earlier byte-identical directory move.
The full unnormalized hashes remain in the manifests.

Evidence in this directory: `ctest.txt`, `ctest.xml`, `focused.txt`, `focused.xml`, `framework.txt`,
`framework.xml`, `solc-oracle.json`, `backend-summary.json`,
`backend-destructuring-summary.json`, `corpus-current.json`,
`corpus-sources.json`, `corpus-summary.json`, `corpus-identity.json`, `corpus-comparison.json`,
`semantic.xml`, `semantic-comparison.json`, `cache-integrity.json`,
`semantic-corrupt-comparison.json`, and `cache-recovery.xml`/`cache-recovery.txt`.
The corpus identity records the merge HEAD while the now-committed fixes were
still in the working tree; its binary hash matches the final runtime compiler.
The preceding baseline manifest is `../builder-layered-layout/corpus-before.json`.
Raw generated outputs stay local and ignored under
`/tmp/puya-sol-context-fixes.xYja6A/`.

## Environment recovery and final repeat

The first full repeat stopped near 90% without a final JUnit report when the
environment was interrupted. A retry encountered KMD connection resets during
session setup and was stopped. Algod remained available, but KMD was absent
from the existing `algokit_sandbox_algod` container. Starting only KMD with
`goal kmd start -d /algod/data --timeout 0` restored wallet access; the eight-case
smoke test passed. No ledger reset or data deletion was performed.

The subsequent full run completed in 424.27 seconds with 2,472 passed,
46 failed, 102 xfailed and 37 xpassed. Apart from the known DCE case, all
failures were invalid cached JSON or TEAL containing NUL bytes. A read-only
integrity scan covered 29,098 entries (all backend entries and source-cache
entries created since this binary was built), validating 133,364 text files
and checking 6,054 bytecode files against compiler-written hashes. It identified
72 corrupt source-cache entries containing 346 damaged text artifacts. These
entries were moved, not deleted, to local ignored
`out/context-cache-quarantine.1shuVZ/`. Healthy cache entries remain in place.
The corruption is consistent with the preceding environment interruption;
the exact filesystem-level cause has not been established.

After quarantine, the 46-case retry passed all 45 affected cases in 13.05
seconds; only the known DCE failure remained. The final full run completed with
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3 --tb=short`.
The final outcome is 2,517 passed, 1 failed, 100 xfailed and 39 xpassed, in
391.74 seconds. The sole failure is the preceding baseline's Puya
DCE/divide-by-zero case; it is not an accepted divergence. The binary remained
unchanged, and neither the ledger nor expected-failure markers was reset.

## Separate experiment work

After this completed `rev-2` validation and results commit, rebase
`experiment/scratch-memory-model` onto it, and finish/validate item 4 there.
The three top-level research notes remain untracked. Raw output files are not
to be added to Git.

Follow-up outside this context audit: cache lookups currently accept the
presence of an ARC-56 file without checking artifact content integrity. Using
the existing compiler-written hashes to reject damaged entries as cache misses
would avoid treating corrupted cached bytes as compiler failures. This recovery
did not change the harness or its cache policy.
