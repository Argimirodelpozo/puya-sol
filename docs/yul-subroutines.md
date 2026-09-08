# Preserving Yul functions — 2026-09-08

This records the outlining baseline. The subsequent
[memory-word sharing and Yul call-fact follow-up](memory-word-subroutines.md)
reduces the verifier further to **16,597 + 4 bytes**, still 217 bytes over the
combined limit. Its latest full-suite results supersede the run below.

Yul functions can remain subroutines. The frontend now emits each reachable
ordinary function once, instead of expanding its body at every call site.
This removes the measured PLONK SSA bottleneck without modifying Puya or
redesigning memory. It does **not** establish that the resulting verifier
fits AVM deployment limits or correctly executes a proof.

## Calling convention and authority

[SolcFacts](../src/builder/SolcFacts.cpp) consumes the vendored solc
disambiguator, function definitions, call graph, recursion analysis, dialect
builtin handles and propagated memory effects. Target-specific decisions
consume those facts; there is no new Solidity/Yul parser or scope resolver.

- All subroutine IDs are registered before any body is lowered. Forward and
  mutually recursive calls are independent of declaration order.
- Library/free-function helpers are collected before contract building resets
  its pending sink, and again after late path specializations. Host-bound
  helpers retain per-contract storage-dispatch scoping.
- Explicit parameters and return values are Yul words. Zero, one and multiple
  returns use void, biguint and tuples respectively; returns default to zero.
- A fresh local builder scope retains host-contract routing and solc facts,
  but not caller-local values, memory-content constants or calldata shortcuts.
  Solc forbids capturing outer stack variables in a Yul function.
- Memory, the free-memory pointer, returndata and transient storage retain
  their existing shared scratch representation. No per-call memory copies,
  writeback protocol, extra memory pages or new allocation model were added.
- A function that directly or transitively accesses calldata receives the
  existing immutable synthetic EVM calldata blob as a hidden argument. It
  does not refer to undefined caller-local calldata variables or rebuild the
  buffer per call. Calldata pointer arguments retain their word values.
- Solc's propagated memory-write effect invalidates the caller's tracked
  memory contents at calls which can write them.
- `leave` returns the current function's return variables, including from
  nested loops and switch cases. The old recursive path silently ignored it.
- All user-function call sites share right-to-left argument evaluation and
  capture nontrivial values before evaluating the next argument. Merely
  translating arguments in that order did not suffice: a queued mutation
  could run before an earlier argument's deferred memory read.

Functions which directly or transitively contain a successfully terminating
EVM builtin retain the inline fallback. In particular, `return(offset,size)`
uses the enclosing Solidity return frame and must not become a Yul `leave`.
The selection is conservative, even for syntactically present unreachable
terminators. Recursive functions in that group receive an explicit error;
supporting their whole-call exit convention remains future work. Reverting
helpers can be outlined because revert already aborts execution.

Puya's existing selective IR inliner still runs **after initial SSA**. This
change preserves source function boundaries through the expensive initial
construction step; it does not promise a final `callsub` for every tiny helper.
The existing `--force-no-inline-sub` option applies to these emitted routines
as well. No new flag or backend inlining heuristic was added.

## PLONK measurement

The input is the cached verified SP1 v6.1.0 PLONK source used by the
[Blobstream campaign](../tests/chainwide-historical-diff/BLOBSTREAM_REPLAY.md).
The EVM-wire-ABI, EVM-slot-storage, xchain and default five-page memory profile
is unchanged. The verifier source and proof logic are not rewritten.

The final implementation produces a complete `SP1Verifier` artifact in a
timed unchanged-Puya probe:

| Measurement | Before outlining | Final implementation |
|---|---:|---:|
| Frontend AWST, same source-root spelling | 469,895,934 bytes | 142,356,970 bytes |
| Fresh frontend time | 40–59 seconds | 11.9–12.5 seconds |
| Backend targeted `SP1Verifier` probe | stopped at 150 seconds, still in initial SSA | complete in 56.2 seconds |
| Contract IR construction | incomplete after 127 seconds | 4.7 seconds |
| Trivial-phi replacement time | 120.8 seconds, incomplete | 6.4 seconds across all function/contract builds |
| Approval / clear bytes | no artifact | 33,879 / 4 |

This is about **70% less JSON** than the same-path baseline. There are 40
outlined Yul helpers per contract and two contract roots. Building all root
subroutines takes another 13.7 seconds in the final probe; that work is
separate from the 4.7-second contract IR row. The final artifact retains 66
static `callsub` sites after Puya's own optimizations. No backend code changed.

The **final, normal full compiler invocation**, compiling both contract
targets through the existing harness with a fresh private cache, completed
in **119.8 seconds** while the three-worker semantic suite ran concurrently.
It produced `PlonkVerifier` at **32,842 + 4 bytes** and `SP1Verifier` at
**33,879 + 4 bytes** (approval + clear). The final derived artifact also
retains 66 static `callsub` sites. The later library-root integration fix
produces byte-identical PLONK AWST, and the final targeted backend probe
produces byte-identical approval bytecode to that normal full invocation.
Its SHA-256 is `b949a3c0dfe02d6bbcbf3ba4e9ecba9eb55ed77ba78b561e1460f178ecbf30b7`.

These are local single-run observations, not a performance guarantee.
The original 900-second failed compilation used a shorter source-root path
and emitted 424,222,548 bytes; repeated source-location strings account for
that difference from the 469,895,934-byte profiling baseline. Both baselines
remain retained. Do not compare the source-path difference as an optimization.

## Verification and remaining limits

[The regression fixture](../tests/solidity-semantic-tests/tests/puyasolRegression/contracts/yul_subroutines.sol)
and [tests](../tests/solidity-semantic-tests/tests/puyasolRegression/test_yul_subroutines.py)
cover mutual recursion, nested and multi-return leave, default return values,
switch isolation, shared memory/free-memory pointer, page-straddling words,
Solidity dynamic/fixed arrays and bytes passed to and mutated by Yul helpers,
direct/transitive calldata reads and copies, all four user-call syntactic
positions, storage rollback, whole-call return fallback, and emitted AWST
function boundaries. A second fixture covers library/free-function helper
emission and storage dispatch across two host contracts. Both named and
EVM-slot storage layouts are exercised.

The six new runtime/shape tests, 19 native CTests and 91 chainwide harness
unit tests pass. The local solc 0.8.34 oracle independently agrees with 39
checks in each of legacy and via-IR modes.

The **final full semantic + harness run** recorded **1,874 passes, 1 failure,
101 expected failures and 38 unexpected passes**, across 2,014 tests in
335.68 seconds. The only failure is the previously recorded
`test_dce_reverting_subexpr_literal_folds`: Puya drops an unused expression
which must revert on division by zero. There are no new failures; this is
not an all-green suite. No xfail/XPASS markers were changed or reclassified.
The 14 tests added since the recorded 2,000-test baseline comprise the six
new Yul cases and eight pre-existing harness/cache tests, not 14 compiler
bugs fixed by outlining.

Command, from `tests/solidity-semantic-tests`:

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 PUYASOL_LOCALNET_RESET=0 PUYA_SOL_NO_SERVE=1 \
  python3 -m pytest tests/ framework/ -q -p xdist.plugin -n 3 --tb=short \
  --junitxml=/tmp/puya-sol-yul-outlining-20260908.1ktuhU/semantic-integrated.xml
```

The compiler remained fixed throughout the run; the existing LocalNet was
not reset. An earlier full run before the root-helper integration fix and
the focused/prototype reports also remain in the local evidence directory.

The final generated verifier exceeds the 16,384-byte combined program
limit. It has not been deployed or executed with the four bridge proofs on
the AVM. Code-size reduction or explicit contract splitting is a separate
next decision; this result does not justify changing the verifier, enlarging
platform limits, substituting a proof, or claiming the bridge replay green.
Call-site constant specialization can be lost when outlining; runtime cost
and emitted size must be measured, not inferred from the smaller AWST.

Local measurements, JUnit reports, the independent EVM oracle and isolated
compile artifacts live under
`/tmp/puya-sol-yul-outlining-20260908.1ktuhU/`. The earlier SSA diagnosis lives
under `/tmp/puya-sol-plonk-profile-20260908.lm5B2p/`. Compiler binary for the
final checks: `b7b266b4963e79dfb95cb19db9abc92b63af63c64f002dc6cec8e9317f221ac6`.
Repository base is `rev-2` at `5dee40fa5a0e8794f4c416729e4b34ed20d17c73`, with
uncommitted changes; the existing chainwide work and untracked
`memory_redesign.md` remain separate.

The `src/` change is **224 added / 150 removed lines, net +74**. This source
change is distinct from the reduction in generated AWST size.
