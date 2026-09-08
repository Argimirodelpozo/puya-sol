# SP1 Groth16 differential replay — 2026-09-08

A targeted new chainwide-harness campaign, not a full-corpus sweep. The fetched
SP1 gateway history agrees between local py-evm and puya-sol on go-algorand's
canonical in-memory evaluator, with real verifier contracts on both legs.
Repository base: `rev-2`, `5dee40fa5a0e8794f4c416729e4b34ed20d17c73`;
the harness changes described below are additional working-tree changes.
No `src/` or memory-model changes were needed. No live-chain transactions or
shared LocalNet changes were made.

## Historical window and actual calls

Target: Ethereum's SP1 Groth16 gateway
`0x397a5f7f3dbd538f23de225b51f532c34448da9b`, identified in
[Succinct's deployment registry](https://github.com/succinctlabs/sp1-contracts/blob/main/contracts/deployments/1.json).
Inputs and verified source trees came from Blockscout. The retained calls span
blocks 21,094,774–25,832,817. A 100-entry limit yielded 32 entries: 26 direct
transactions and six calls recovered from seven selected parent traces, with
zero unavailable selected traces. This is not evidence that unindexed,
eventless inbound calls were exhaustively discovered.

The stream contains five `addRoute`, two `freezeRoute`, one `transferOwnership`,
and 24 `verifyProof(bytes32,bytes,bytes)` calls. All 32 succeed on both local
VMs, matching their historical statuses, with no closed-world skips and no
AVM platform-limit exclusions. The differ reports no divergence in compared
statuses, successful return values, events, snapshots, probes, or storage.
There are two parameterized route probes, both compared successfully.

All five dependencies were compiled and deployed as real `SP1Verifier`
contracts: no generic fallback was used and `oracle.dep_tapes` is empty.

| Version | Historical verifier address | AVM app | Observed inner calls |
|---|---|---:|---:|
| v6.1.0 | `0xb69f2584cbcff99a58c4e7002e8b89af54a6f4e2` | 9002 | 25 |
| v4.0.0-rc.3 | `0xa27a057cab1a4798c6242f6ee5b2416b7cd45e5d` | 9003 | 1 |
| v5.0.0 | `0x50acfbedecf4cbe350e1a86fc6f03a821772f1e5` | 9004 | 1 |
| v6.0.0 | `0x99a74a05a0fabeb217c1a329b0dac59a1fa52508` | 9005 | 1 |
| v3.0.0 | `0xe780809121774d06ad9b0eeec620ff4b3913ced1` | 9006 | 1 |

Of these 29 calls, five query `VERIFIER_HASH()` during registration and 24
verify actual proofs through v6.1.0 (proof selector `4388a21c`). The other four
versions' proof-verification paths were not exercised by this history.

## Local rejection controls

A separate, explicitly synthetic case repeats the same history, then adds:

1. The last valid proof with changed public values: both reject.
2. An unknown verifier selector: both reject.
3. A truncated proof: both reject.
4. The unchanged valid proof again: both accept, including its real verifier call.

That case completes 36/36 calls: 33 successful, three rejected, no skips or
platform exclusions, and no reported divergences. It is the original 32 plus
four controls, not 36 additional historical transactions. Synthetic entries
and their expected outcomes are marked in `case.json`; those expectations are
not Ethereum receipts. Base proof transaction:
`0x10285409e9387ef7c53b86391a699bf44ac81c5fccc7302eaec8ae1956c97950`.

Rejection agreement is status agreement, not byte-identical revert data. The
per-contract differ treats two rejections as a match; py-evm reports EVM error
bytes while the AVM reports evaluator errors. Failed-group inner calls may be
absent from `inners_after`, so their empty `inner_apps` must not be counted as
evidence of no attempted callee execution.

## Scope limits

- **Storage is not fully covered.** Both SP1 reports identify three unmatched
  AVM boxes and three written EVM slots not read by the differ. These correspond
  to route keys produced by verifier hash queries, rather than explicit
  `freezeRoute(bytes4)` inputs. Two historical route keys are compared, three
  are not. There is no holder-coordinate mismatch. Extending discovery with
  solc-typed event/return values is a useful next step; do not infer full
  storage equivalence from the zero-divergence count.
- **The historical window covers one real external hop.** Gateway → verifier is an actual inner
  AVM application call. The verifier's
  [`this.verifyProof(proof, inputs)`](https://github.com/succinctlabs/sp1-contracts/blob/main/contracts/src/v6.1.0/SP1VerifierGroth16.sol)
  uses puya-sol's existing self-call-to-subroutine adaptation, acknowledged by
  `--allow-divergence self-call`. This is not native self-reentrancy or an
  end-to-end historical application → gateway → verifier campaign. The separate
  synthetic application campaign below now covers two real external hops. Governance callers
  recovered from traces are impersonated target-entry senders; their complete
  Safe transaction graphs are not deployed/replayed.
- **Research profile.** Named-cell storage, EVM wire ABI, the harness's xchain
  address adaptation and divergence acknowledgements, and
  `--legacy-source-rewrite` were used. All 20 source entries in the six compile
  manifests differ only by Solidity pragma relaxation; there were no verifier
  body rewrites. EVM compilation used solc 0.8.20 for root and dependencies.
  Multi-file support does not add per-dependency solc-version selection.
  Cross-contract static read-only enforcement remains the accepted,
  warning-only divergence.
- **Budget is amplified.** Every oracle call uses the existing 16-transaction
  resource group. All 24 historical proof calls required the OpUp-amplified
  tier (roughly 94k opcode-budget envelope). This is not a measurement of
  consumed opcodes, fees, or a claim that the default single-call budget suffices.

## Harness fixes and regression checks

- `fetch.py` no longer shadows its `internal` boolean with legacy index
  results. Previously an empty legacy index disabled the requested raw-trace
  recovery, hiding the gateway's governance setup and causing proof skips.
- Root and dependency fetching now share verified multi-file source-tree
  materialization, including remappings and relative-path validation. Both
  VM legs compile these dependencies from the real source tree; the AVM
  compiler gets a disposable copy, preserving fetched originals.
- Oracle app creation uses the compiled ARC-56 `__postInit` declaration to
  distinguish deferred constructors. A deferred creation no longer names
  unused dependency apps, whose program-read cost caused a resource failure
  with all five verifiers registered. Immediate constructors retain the
  existing dependency references; deferred execution still budgets its callees.
- Oracle results record recursive per-app inner-call counts, separate from
  helper IDs and from speculative failed attempts.

The initial 12 targeted tests passed. After the three-contract extension below,
all 24 targeted tests pass. The first full harness run had 76 passes and one
unrelated CCTP shim-test failure: the shared artifact cache held an EVM-profile
transmitter while the test expected ARC-4 router labels. The follow-up isolates
joint CCTP builds in `out_avm_joint` and makes that test independent of generated
artifacts, with explicit required shims and fail-closed regression cases. The
full harness suite now has **91 passes, zero failures**. This is not a fresh
certification of the full CCTP history; see the
[joint replay documentation](README.md#oracle-backed-joint-cctp-replay) for its
artifact and compatibility boundaries.

The storage-differ self-test was also rerun on fresh oracle ledgers in both
named-cell and EVM-slot modes: each completes 17/17 calls and 59/59 probes,
with no divergences. `selftest.check` confirms all seven mapping shapes contain
at least two entries on both VMs. The full compiler semantic suite was not
rerun for these harness-only changes.

## Local artifacts and rerunning

New case directories are ignored generated artifacts; no existing case report
was overwritten:

- [Historical report](cases/zk_sp1_groth16_joint/report.json), with fetched
  sources, `case.json`, `calls.json`, both VM results and compilation manifests
  in the same case directory.
- [Synthetic-control report](cases/zk_sp1_groth16_controls/report.json), with
  synthetic-control provenance in its `case.json`.
- Storage self-test runs remain under
  `/tmp/puya-sol-chainwide-20260908.xorLzC/selftest_default` and `selftest_slot`.

From `tests/chainwide-historical-diff`, rerun either cached SP1 case:

```bash
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python evm_leg.py \
  cases/zk_sp1_groth16_joint '{"max_txns":100,"snapshot_every":5}'
PUYA_SOL_NO_SERVE=1 python3 oracle_case.py cases/zk_sp1_groth16_joint
python3 differ.py cases/zk_sp1_groth16_joint
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q .
```

Substitute `zk_sp1_groth16_controls` to rerun the controls. The AVM command
requires the existing avm-prover oracle binary (or `--oracle /path/to/avmoracle`).
`PUYA_SOL_NO_SERVE=1` avoids a stalled persistent compiler-server path observed
in this sandbox; it does not change the Solidity sources or replay profile.
Plugin autoload is disabled because the globally installed pytest rerun plugin
opens sockets that the sandbox disallows.

To fetch a new window, use a **new tag** so these recorded results stay intact:

```bash
python3 fetch.py eth.blockscout.com \
  0x397a5f7f3dbd538f23de225b51f532c34448da9b zk_sp1_groth16_new \
  --max-txns 100 --internal --internal-parents 30
```

Future fetches can contain newer inputs; the counts above apply only to the
recorded window.

## Three translated contracts: two genuine external hops

The follow-up [three-contract driver](sp1_three_contracts.py) compiles and
deploys the following contracts on both local VMs:

```text
ProofApplication (app 9001; 1,027-byte approval)
  -> SP1VerifierGateway (app 9003; 1,681-byte approval)
      -> SP1Verifier v6.1.0 (app 9002; 15,512-byte approval)
```

[ProofApplication](fixtures/ProofApplication.sol) is explicitly a synthetic,
stateful consumer, not a historical deployed application. It binds the program
key in its constructor, hashes public values, and increments a counter and
stores a digest **before** calling the gateway. A failed inner call must undo
both writes. The gateway and verifier are the actual verified SP1 source
trees, with only pragma relaxation in all nine source-manifest entries.
There is no stub or answer-tape fallback. The gateway is configured through
its real constructor and `addRoute`, not through fabricated storage writes.

The [fresh paired report](cases/zk_sp1_three_contracts_verified/report.json)
contains **28/28 matching outcomes, zero findings**:

- All 24 historical proof inputs succeed through the synthetic application.
- Changed public values, an unknown verifier selector, and a truncated proof
  are rejected on both VMs; the application's counter and digest remain unchanged.
- The unchanged valid proof succeeds afterward, taking the counter to 25.
- Every successful call has a recorded gateway node with a verifier **child**
  on both VMs. Sibling calls would fail this check. The 25 successful AVM
  submissions therefore contain 50 real inner application calls, excluding
  setup and inspection calls. The verifier's additional self-call remains
  the already acknowledged subroutine adaptation.
- Returned digests, application counter/digest state, and the registered route
  address/frozen flag match independent expected values after every call.
  Resource/budget failures cannot pass as expected proof rejections.

This is an application integration fixture, not a historical full-bridge
replay or exhaustive raw-storage comparison. Duplicate submission of a valid
proof is intentionally allowed by this fixture; bridge-specific message
consumption and replay protection have not been modeled. Named-cell storage,
EVM wire ABI, the existing staticcall/self-call adaptations, and the oracle's
16-transaction/OpUp resource profile remain in use. All 25 successful proof
submissions need the amplified tier.

The test exposed another oracle-harness defect: an immediate constructor in
EVM-ABI mode expects one ABI-encoded tuple, while the ARC-4 deployment helper
supplied one application argument per value. `creation_app_args` now uses
solc's constructor ABI and the compiled `__postInit` lifecycle declaration to
select the correct encoding. Deferred constructors retain their existing
ARC-4 path. No compiler-source fix was necessary. Both storage self-tests
were rerun after this fix (17 calls / 59 probes each; zero divergences and all
seven shapes populated).

From this directory, use a new output directory for each fresh paired run:

```bash
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python sp1_three_contracts.py evm \
  cases/zk_sp1_groth16_joint cases/zk_sp1_three_contracts_new
PUYA_SOL_NO_SERVE=1 python3 sp1_three_contracts.py avm cases/zk_sp1_three_contracts_new
python3 sp1_three_contracts.py diff cases/zk_sp1_three_contracts_new
```

The output keeps copied Solidity sources, solc ABIs/storage layouts, compiled
AVM artifacts, the precise input/control list, and per-call state and call
trees. The EVM preparation refuses to overwrite an existing directory.

## ZK bridge follow-ups

**SP1 Blobstream now has a partial checkpoint replay.** The
[bridge campaign report](BLOBSTREAM_REPLAY.md) records four actual proofs
passing on the EVM and successful AVM bridge initialization, but the deployed
PLONK verifier now compiles after Yul outlining but exceeds AVM program-size
limits. This is not a completed
bridge differential replay. Its actual light-client contract
checks the trusted header and target range, verifies the proof, then updates
Celestia header/commitment mappings and a proof nonce. This replaces the
synthetic consumer with a real stateful application using SP1. It is a
data-attestation bridge, not a token bridge. The campaign pins an independent
historical checkpoint, program key, verifier version, and matching proof
inputs; arbitrary SP1 proofs from the current fixture are not Blobstream
proofs. Proxy-runtime initialization is explicit; upgrades remain untested.
[Source](https://github.com/succinctlabs/sp1-blobstream/blob/main/contracts/src/SP1Blobstream.sol),
[project overview](https://github.com/succinctlabs/sp1-blobstream).

**A full message/token bridge is a separate, larger campaign.** Polyhedra's
zkBridge exposes send and receiver delivery interfaces, including destination
`zkReceive` calls. A useful translation/replay should include the relevant
light-client/verifier, delivery endpoint, and real receiver/token contracts,
not just the proof checker. Initialization and matching source-chain messages
are prerequisites. Local correctness checks should cover message consumption,
duplicate rejection, state transitions, and rollback across delivery failures.
Those are proposed tests, not claims about behavior already validated here.
[Official messaging interfaces](https://docs.polyhedra.network/expchain/zkbridge/).
