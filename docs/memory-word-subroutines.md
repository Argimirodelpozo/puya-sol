# Memory-word sharing and Yul call facts — 2026-09-08

The two follow-ups to [Yul outlining](yul-subroutines.md) are implemented.
The real SP1 PLONK verifier falls from **33,879 to 16,597 approval bytes**:
**17,282 bytes saved (51.01%)**. Its four-byte clear program brings the total
to **16,601**, still **217 bytes over** the 16,384-byte combined limit.
No AVM proof execution or deployment is claimed.

## Changes

1. Generic memory-word reads and writes now share one subroutine each.
   Literal offsets and proven 32-byte-aligned accesses retain their inline
   lowering. The helpers use the same scratch slots, page-straddling logic,
   bounds checks and word representation. Both are non-pure, including the
   reader: a preceding store must remain observable. Both disable inlining.
   Helper roots are collected once per compilation, including uses from
   libraries, multiple contracts and late path specializations.
2. Immutable Yul parameters inherit constants and modulo-32 residues that
   hold at every reachable call site. Scope, reachability, assigned variables,
   immutable definitions, movability and constant/offset reasoning come from
   solc's disambiguator, call graph, SSAValueTracker, SideEffectsCollector,
   VariableReferencesCounter and KnowledgeBase. A small target-side residue
   calculation follows those definitions; a fixed point carries facts across
   nested calls. There is no function cloning or pointer-width change.

Assigned parameters and definitions capturing mutable variables are excluded.
Conflicting, unknown and unprovable recursive arguments remain generic.
The new pass does not infer facts about values read from memory, including
`mload(0x40)`. It does not extend the existing free-memory-pointer analysis.

The second step adds **zero additional bytes saved on this verifier**.
Its memory-derived helper pointers remain unknown to the new pass. Step-one
and step-two AWST and approval binaries are byte-identical. Regression shape
tests establish that the pass does remove generic accesses where common
constant/alignment facts are available; that is not evidence of a PLONK gain.

No memory layout, allocation model, verifier source/proof logic, Puya code,
platform limit or scratch-memory capacity changed. `AsmScan.h` and the
untracked `memory_redesign.md` remain untouched. Precompile store forwarding,
fixed revert-payload sharing and explicit contract splitting are not included.

## Measurements

Input: the cached verified SP1 v6.1.0 PLONK source from the
[Blobstream campaign](../tests/chainwide-historical-diff/BLOBSTREAM_REPLAY.md),
with unchanged EVM-wire ABI, EVM-slot storage, xchain and five-page memory
profile. The existing compatibility-pragma rewrite and its manifest are
unchanged. AWST comparisons below use the same source-root spelling.

| Measurement | Before these changes | Step 1 | Step 2 |
|---|---:|---:|---:|
| SP1Verifier approval bytes | 33,879 | 16,597 | 16,597 |
| Clear bytes | 4 | 4 | 4 |
| AWST bytes | 142,356,970 | 62,633,831 | 62,633,831 |
| Targeted backend wall time | 56.2 s | 16.3 s | 13.1 s |
| Static cross-page read branches | 109 | 1 | 1 |
| Static cross-page write branches | 162 | 1 | 1 |
| Stack-shuffle instruction bytes | 14,309 | 5,546 | 5,546 |

The shared reader is 60 bytes and the writer 82 bytes. These are emitted
code-size measurements, not runtime costs. The analyzer decodes actual
bytecode using pinned Puya opcode definitions and checks the final byte
offset against the binary length; source-text line counts are not byte sizes.
Branch grouping is based on generated TEAL block structure.

The normal full compiler invocation builds both contracts in **52.6 seconds**
while the semantic suite runs: `PlonkVerifier` is **15,541 + 4 bytes** and the
derived `SP1Verifier` is **16,597 + 4 bytes**. The derived approval artifact is
byte-identical to the targeted probe. These are single-run observations;
the step-two timing difference is noise, not evidence of an optimization
gain over identical step-one AWST.

## Verification

- Focused memory/Yul regressions: **12 passed**. New cases cover one, two and
  five memory pages; neighboring bytes; repeated read/write effects; page
  straddles; last valid and first invalid word offsets; constant/aligned,
  conflicting, reassigned, snapshotted, recursive and memory-loaded arguments.
  Yul call facts are exercised with named and EVM-slot storage profiles.
- Independent local solc 0.8.34 EVM oracle: **66 checks per mode**, both legacy
  and via-IR (**132 passed**). AVM scratch-capacity rejection is deliberately
  not imposed on EVM memory.
- Native CTests: **19/19 passed**. Chainwide harness unit tests: **91 passed**.
- Full semantic and harness suite: **1,880 passed, 1 failed, 101 xfailed,
  38 xpassed**, across **2,020 cases** in **416.38 seconds**, three workers.
  All 2,014 prior cases retain their JUnit outcome; six new cases pass and no
  cases were removed. The sole failure remains
  `test_dce_reverting_subexpr_literal_folds`, the known pinned-Puya bug that
  drops an unused divide-by-zero expression. This is not an all-green suite
  or an accepted divergence. No xfail/XPASS markers were reclassified.

The compiler binary stayed fixed during the full run and the existing
LocalNet was not reset. Proof replay, runtime proof budget, and both bridge
inner-call hops remain untested on this PLONK artifact.

## Evidence

Local artifacts, scripts and reports are under
`/tmp/puya-sol-memory-size-20260908.3MgBFL/`: `frontend-step1/`,
`frontend-step2/`, `backend-step1/`, `backend-step2/`, `full-compile/`,
`step1-size.json`, `step2-size.json`, `semantic-final.xml`, `chainwide.xml`,
and `evm_fixture_check.json`. The baseline analyzer and byte accounting are
under `/tmp/puya-sol-plonk-size-20260908.lk0ZVE/`.

Compiler SHA-256 for step one:
`b7d633e3a1cc020e9cadcd2395ae100576e3f9c618682ed4d8e91ab288671e8e`.
Compiler SHA-256 for step two and the full suite:
`914afa3dce0c73902cd4e800877d68c31cee9ddd00468bbad1236d61c12ae34a`.
Derived approval SHA-256, both steps and the normal full compile:
`0094bb82344924bd70022164a6da648a7e1d6c4345f3a0a96b61f822fe2e1692`.

Base remains `rev-2` at `5dee40fa5a0e8794f4c416729e4b34ed20d17c73`, with
uncommitted changes. Source-code growth is separate from generated-program
reduction: this follow-up adds **247 net lines in `src/`**, on top of the
earlier Yul-outlining change's net 74 lines.
