# chainwide-historical-diff

Replay the **real historical transaction sequence** of a deployed (verified)
contract against two local legs, in lockstep from contract creation, and diff
them:

- **EVM leg** — local py-evm re-execution (eth-tester), multi-sender, real
  constructor args, per-txn historical timestamps. This is the **oracle**.
- **AVM leg** — the same source compiled by puya-sol, deployed on LocalNet,
  driven through the same decoded call sequence with the same (mapped) senders.
  Compilable in either storage model: the default **named-cell** model (each
  state var in its own box/app-global, keys derived by hashing the variable
  name) or, with `--evm-layout`, the **EVM slot** model
  (`--evm-storage-layout`: one flat `uint256 slot → bytes32` space backed by
  boxes). The slot model makes the storage diff *slot-for-slot* — the same
  layout on both legs, so comparison stops being name-based alignment.

The chain itself is **not** the oracle — it is (a) a source of realistic inputs
(real call sequences, real senders, real argument distributions, real revert
paths, long-horizon state) and (b) a **closed-world filter**: any txn whose
local-EVM status disagrees with its historical receipt status must have touched
external state (other contracts, balances, block env) and is skipped
**symmetrically on both legs**, keeping the two states in lockstep.

## Why this finds bugs ordinary fuzzing misses

- Long-horizon state: thousands of organic txns grow arrays/checkpoints/boxes
  far beyond what generated sequences reach.
- Real orderings (approve→transferFrom races, dust amounts, max-uint approvals).
- Real revert paths, diffed including payloads.
- Real constructor args and real hardcoded-address interactions.

## Usage

```bash
# one-shot: fetch (cached) + replay + diff
python3 replay.py pepe --host eth.blockscout.com \
    --address 0x6982508145454Ce325dDbE47a25d4ec3d2311933 --max-txns 300

# pieces
python3 fetch.py eth.blockscout.com 0x6982...1933 pepe --max-txns 300
python3 replay.py pepe            # uses cases/pepe/

# EVM-COMPAT MODES: compile the AVM leg with --evm-storage-layout (flat EVM
# slot space in boxes) and/or --evm-memory-layout (universal blob memory).
# In slot mode the storage diff becomes SLOT-FOR-SLOT (see chd_slot_reader).
python3 replay.py usde --evm-layout                 # FULL EVM semantics (storage+memory+transient)
python3 replay.py morpho --evm-layout               # (--evm-memory is folded into --evm-layout now)
python3 batch.py --evm-layout --max-txns 200 --only usde,degen

# Before a batch: rewind LocalNet's clock so cases replay at TRUE historical
# time. Each replay ratchets it forward by that window's span and it never
# comes back, so a long batch drifts years into the future (see the clock note
# under "Scope & constraints"). Nothing in this suite persists on LocalNet.
algokit localnet reset

# INTERNAL (contract-to-contract) CALLS merged into the stream — for a
# router-traded token this is most of the real traffic (see below).
python3 fetch.py eth.blockscout.com 0x6982...1933 pepe --max-txns 200 --internal

# Known router/parent cases can seed value-free internal calls directly.
# The optional final number caps matches from each case/selector, and coverage
# (including unavailable parent traces) is persisted in case.json. Fetch the
# named parent cases first; their cached transaction lists supply the hashes.
python3 fetch.py eth.blockscout.com 0xc492...e907 cctp_minter \
    --max-txns 200 --internal --internal-parents 12 \
    --script-deps --script-traces 0 --relax-pre08 \
    --parent-case-selector cctp_messenger:0x6fd3504e:4 \
    --parent-case-selector cctp_transmitter:0x57ecfd28:8

# candidate harvesting: by holders (default) or by protocol TVL
python3 harvest.py --tokens ERC-20
python3 harvest.py --tvl 40

# self-test the STORAGE DIFFER on a synthetic contract (no network)
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python selftest.py            # named-cell model
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python selftest.py --evm-layout
```

`selftest.py` exists because a real history only exercises the storage shapes
that contract happens to use, and a map both legs read as *empty* looks
identical to a map both legs read *correctly*. It replays a synthetic contract
whose calls are guaranteed to populate every shape the readers claim to
support — scalar, nested, struct- and array-valued mappings — and fails if any
of them comes back empty. It found two real defects the day it was written (an
undecoded nested map, and array/struct maps that were never discovered at all).

Requires: LocalNet running, `build/puya-sol` built, and the
`tests/WIP/tiny-fuzzing-oracle/.evmvenv` venv (web3/eth-tester/py-solc-x) —
the EVM leg runs under that interpreter as a subprocess.

## Oracle-backed joint CCTP replay

`oracle_cctp_historical.py` replays the cached `cctp_transmitter`,
`cctp_messenger`, and `cctp_minter` histories in one chronological avm-prover
ledger. Unlike the ordinary per-contract replay, the three unsplit contracts
call each other as real AVM inner application calls. Twelve trace-derived
entries are therefore excluded from the 429 root calls rather than being
executed twice.

```bash
python3 oracle_cctp_historical.py cases \
  --prover-root /path/to/avm-prover \
  --output /tmp/cctp-historical.json
```

The driver uses unified `Access` resources first and falls back to a pooled
16-transaction resource group when needed. Historical contract addresses keep
their exact signed 32-byte representation; their low 64 bits are also used as
the corresponding application IDs, matching puya-sol's current contract-call
address convention. Timestamp, round, sender, value, constructor order, and
receipt success/revert status are replayed. The report stops at the first
divergence by default; `--continue-after-divergence` marks the suffix tainted.

The complete campaign was validated on 2026-08-17: all 429 root calls were
executed, all 429 AVM statuses matched their Ethereum mainnet receipts, and no
call was skipped.

The same driver was also validated against a disposable wider corpus containing
the first 500 MessageTransmitter calls, the first 500 TokenMessenger calls, and
all 29 direct TokenMinter calls. All 1,029 root calls matched, with zero skipped
calls and zero mismatches. The wider receipt audit found one additional stale
Blockscout success label, retained in `MAINNET_RECEIPT_METADATA` alongside the
three already known corrections.

The result is intentionally a **CCTP receipt-status replay**, not a claim of
full Ethereum-state equivalence:

- USDC is the corpus' `StubERC20`. Before each historically successful
  `depositForBurn`, the runner records and performs a synthetic mint and
  approval. It does not prove historical USDC balances or allowances.
- Return bytes, event payloads, and full EVM/AVM storage snapshots are not
  compared yet.
- The cached CCTP sources were Solidity 0.7.6 but were fetched through
  `--relax-pre08`. Disposable copies of the cached TEAL therefore apply four
  reported, CCTP-specific compatibility shims: TypedMemView's intentional
  `uint8(32 * 8)` wrap, OpUp budget for receive/replace, forwarding the exact
  signed message body, and comparing a caller application to the low 64 bits
  of its historical EVM address. This is not general Solidity 0.7 emulation.
- Four stale success labels in the Blockscout fixtures are overridden by
  explicitly listed Ethereum mainnet receipts. The first 500 calls to each of
  Transmitter and Messenger were re-audited; the cached fixture files remain
  untouched.
- The runner consumes the cached artifacts. Rebuilding the current relaxed
  MessageTransmitter source is presently a separate compiler failure
  (`unresolved root subroutine id '__solfn_1385'`) and is not hidden by this
  replay.

### Corrupt "ok" receipts inside duplicate-payload groups

A relayer race puts the SAME attested message on chain twice. CCTP's
`usedNonces` means the chain accepted exactly one of them — but the indexer
can report "ok" for both, and the replay (which accepts whichever copy it
sees first) then looks like it diverged.

The driver corrects this from evidence, never by assumption. Within a
byte-identical payload group whose outcome multiset does not match history,
an "ok" row whose receipt carries **zero logs** is corrected to failed: a
successful CCTP call always emits (Mint + Transfer + MintAndWithdraw +
MessageReceived), so a logless success is self-contradictory. Every
correction is recorded per row in `scope.zero_log_receipt_corrections` with
its hash and the evidence, alongside the hand-audited
`MAINNET_RECEIPT_METADATA` entries. The correction can only ever turn a
claimed success into a failure, only inside a duplicate group, and only with
the logs to prove it — guarded by three tests in
`test_oracle_cctp_historical.py`.

Worked example (transmitter calls 798/799, sourceDomain 3 nonce 682, three
minutes apart): 798's receipt has zero logs, 799's has all four, so the chain
rejected 798 and accepted 799 while the replay does the reverse. Ordering is
gas-environmental; neither leg models it.

`triage_joint.py <report>` sorts whatever is left into buckets —
`avm_rejects_ok` first, since that is the class that contains real compiler
bugs — and is the fastest way to read a deep window's status mismatches.

Use `--no-pre08-compat` to demonstrate the unmodified pragma-relaxed artifact's
known semantic failure. Unit tests for the stream, corrections, address model,
and compatibility-patch guards live in `test_oracle_cctp_historical.py`.

### Mid-history upgrades

A joint config may carry an `"upgrades"` list; each entry swaps a case's
implementation at its historical upgrade point, in stream order:

```json
"upgrades": [{
  "tag": "cctp2_transmitter",
  "block": 21500000, "txindex": 12, "ts": 1730000000,
  "hash": "0x<upgradeToAndCall txn>", "impl": "0x<new implementation>",
  "contract": "MessageTransmitterV2",
  "avm_artifact": "cctp2_transmitter/out_avm_v3",
  "abi": "cctp2_transmitter/abi_v3.json",
  "src": "cctp2_transmitter/src_v3",
  "multifile": null,
  "ctor_args": [],
  "init_sig": "initializeV3(address)", "init_args": [{"__dep__": "0x..."}],
  "sender": "0x<historical upgrade sender>"
}]
```

- **AVM leg**: the app spec is re-registered with the new artifact's approval
  program — boxes and globals persist, which is byte-for-byte what an
  admin-signed native `UpdateApplication` does (proxy.md §1). The era's
  ARC-56 switches with it, so later calls encode against the live
  implementation.
- **EVM leg**: the new implementation is compiled and scratch-deployed in
  phase A (its constructor runs, baking immutables into the runtime code);
  at the upgrade point the chain is rebuilt with that code at the historical
  address, storage preserved — the same snapshot mechanism as re-homing.
- **Both legs** then replay `init_sig`/`init_args` (the historical
  `upgradeToAndCall` embedded calldata), and the differ merges the new era's
  ARC-56 events and solc ABI into its decode indexes.

**Harvesting is automatic.** `fetch.py` scans the proxy's
`Upgraded(address)` logs whenever `--source-from` is given (opt-in via
`--upgrades` otherwise; both indexed and non-indexed event forms are parsed
— OZ vs the older Zeppelin AdminUpgradeabilityProxy, e.g. USDC). Every
event up to and including the first attachment of the `--source-from`
implementation is the initial era — the attachment can post-date the
creation block on factory-deployed proxies (CCTP v2), and treating it as an
upgrade would double-replay initialize. Later in-window events are
materialized into `cases/<tag>/upgrade_<i>/` (source tree, ABI, ctor args,
the upgrade txn's calls into the proxy) and recorded in
`cases/<tag>/upgrades.json`; a direct `upgradeTo`/`upgradeToAndCall` txlist
scan backstops logless hosts. Then:

```bash
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python gen_upgrades.py <tag> \
    --into joint_config_X.json
```

decodes each era's init calldata and ctor args into marker form, compiles
the era with puya-sol `--evm-layout` into `upgrade_<i>/out_avm`, and merges
ready-to-run entries into the joint config. An unverified era aborts the
generator — the replay cannot cross an upgrade whose source is unknown;
narrow the window instead.

`selftest_joint_upgrade.py` proves the path end-to-end with a synthetic
two-era contract whose upgrade changes all three lanes (a V2-only revert, a
V2-only event, doubled credits in storage), through BOTH the hand-written
config and the fetch-shaped `upgrades.json` → `gen_upgrades.py` pipeline;
it must finish with 0 findings.

## Architecture

```
fetch.py     Blockscout (keyless): verified source (single- or MULTI-file tree
             + remappings) + ABI + constructor args + the contract's own
             verified solc settings + ASCENDING txn history. Optionally
             (--internal) merges contract-to-contract CALLS recovered from
             per-transaction traces, and fetches+stores CONSTRUCTOR
             DEPENDENCIES (verified contracts the ctor args point at).
             → cases/<tag>/{case.json, source.sol, prepared.sol, src/, deps/}
evm_leg.py   [.evmvenv python] decode ctor+txn calldata via ABI → address
             registry → replay on eth-tester (multi-sender, time_travel) with
             an internal closed-world convergence loop (local status vs
             historical receipt status; mismatch → skip → rerun; fast).
             → registry.json, calls.json, evm_results.json
avm_leg.py   compile prepared.sol with puya-sol, deploy on LocalNet (real ctor
             args), fund one Algorand account per historical sender
             (deterministic keys), replay the same calls (per-call
             localnet.account swap → true multi-sender), sim-first to capture
             reverts safely, execute to commit. Platform-limit failures
             (opcode/box budgets) are reported for symmetric re-skip.
             → avm_results.json
chd_slot_reader.py
             [--evm-layout only] rebuilds a slot→word map from the app's page
             ("p:"++itob(slot/64), 64 slots per 2048-B box) and sparse
             ("s:"++slot32) boxes, then walks solc's OWN storageLayout with the
             same forward keccak derivations the EVM reader uses. Output shape
             is identical to the EVM leg's, so differ.py compares them
             unchanged — and coverage is stronger, because dense pages
             enumerate every nonzero slot rather than only derivable names.
differ.py    per-txn status/return/event diff + periodic zero-arg-getter state
             snapshots + final snapshot; address values canonicalised to
             registry symbols («i», «C»=creator, «Z»=zero, «self»); known-noise
             whitelist (e.g. DOMAIN_SEPARATOR()). Events are scoped to the
             contract under test on both legs; dependency-app logs are not
             mixed into the target contract's result.
replay.py    orchestrator: evm_leg ⇄ avm_leg loop (an AVM platform-limit skip
             re-runs the EVM leg with that txn excluded so states stay in
             lockstep) → differ → cases/<tag>/report.json
```

## Address model

Every historical address is mapped through one registry, applied identically to
constructor args, call args, senders, return values and event args:

- **creator** → each leg's default deployer (`«C»`) — so `owner = msg.sender`
  contracts keep working.
- **senders** (any address that ever sent a txn, incl. contracts like DEX
  pairs — locally they're just funded EOAs/accounts with deterministic keys)
  → `«0»,«1»,…`.
- **arg-only addresses** (appear only inside calldata) → deterministic content
  addresses `«10000+»`.
- zero address → `«Z»`; the contract itself → `«self»`.

The same logical symbol resolves to each leg's concrete form on input and is
folded back to the symbol on output, so diffs compare symbols, never raw
chain-specific addresses.

## Internal (contract-to-contract) calls

`fetch.py --internal`. For a token that trades on a DEX, most state evolution
arrives as calls from a router or pair — invisible to `txlist`, which lists
only EOA-initiated transactions. Two traps make this harder than it looks, and
both cost a wrong conclusion before the working recipe emerged:

1. The **address-level** internal-txn APIs return `input: "0x"` (the v2 one has
   no input field at all), which reads as "explorers don't expose calldata".
   They don't — but the **per-transaction** endpoint
   `/api/v2/transactions/{hash}/raw-trace` does, in Parity form
   (`action:{from,to,input,value,callType}`, `traceAddress`, `type`).
2. `txlistinternal` then finds no calldata-bearing calls *at all*, because
   explorers index "internal transactions" as **value-moving traces only**
   (496/496 of PEPE's are plain ETH transfers). Contract-to-contract CALLS are
   not in that index at any depth.

Two parent indexes are used, covering complementary traffic:

- **token-transfer log** (`tokentx`) — for tokens; every internal
  `transfer`/`transferFrom` emits a Transfer event carrying its parent hash.
- **event log** (`getLogs`) — for everything else: a NON-token contract
  (oracle, registry, governance target) never appears in tokentx, but every
  state-changing call it receives tends to emit, and the log index carries the
  parent hash. AaveOracle: 0 direct txns, 0 tokentx parents, 118 logs across
  72 parents.

The working parent index for tokens is the **token-transfer log**: every
internal `transfer`/`transferFrom` emits a Transfer event carrying its parent
tx hash. So: `tokentx` parents in the window, minus known direct txns → pull
each parent's raw trace once → lift every **non-root** `call` entry targeting
us → merge into the stream ordered by (block, txIndex, trace position).

Two properties worth knowing:

- **Parents are successful by construction.** A reverted transaction emits no
  logs, so anything discovered through a Transfer event committed on-chain —
  which is what makes it sound to flatten one atomic transaction into several
  top-level calls. A sub-call that failed inside a successful parent
  (`try/catch`) still replays as an expected failure, since `hist_ok` comes
  from that trace entry's own error field.
- **Callers are NOT deployed.** An internal call is replayed as a direct call
  with the caller's address registry-mapped like any other sender. The thing
  under test is our contract's response to given inputs, and both legs get
  byte-identical calldata and the same `msg.sender` symbol. Contracts the
  target *calls out to* are different — see below.

**Dependency discovery.** Contracts our target calls are fetched, compiled and
deployed on both legs, so the call really happens (an inner txn on the AVM
side). Candidates come from three places, in confidence order:

1. **Observed callees** — `to` addresses of trace entries whose `from` is our
   contract. This is evidence the contract genuinely calls them, and it costs
   nothing extra for internal-call parents (same traces) plus a small sample of
   direct txns. Deliberately *not* "every address appearing in an argument":
   those are overwhelmingly transfer recipients, and fetching them would burn
   the rate limit on EOAs.
2. Constructor arguments that are verified contracts.
3. Hardcoded address literals in the source (the memecoin pattern: factory and
   router baked in, zero constructor args).

Verified, single-file, ^0.8 dependencies can be deployed directly. With
`--script-deps`, an observed runtime callee can instead become a stand-in even
when the main constructor has no dependencies (TokenMinter's USDC shape), with
historical return data served from a selector-aware tape on both legs. The AVM
leg additionally needs a directly deployed dependency to compile with puya-sol
(it reports and continues when it doesn't).

**A deployed dependency supplies code, not history.** This is the real boundary
of the closed-world filter, and it limits how much discovery can ever buy.
BMEX's trace harvest finds its `Vesting` contract and the oracle deploys it
cleanly — and BMEX still replays 4/200, because a *fresh* `Vesting` holds none
of the schedules that years of real transactions put there, so the calls revert
exactly as before. Discovery converts skips into coverage only when the
dependency is effectively stateless (a library, a router, an oracle read) or
when its state is built by the same replayed window. A dependency whose state
predates the window cannot be recovered by deploying it.

Effect on PEPE: **+158 internal calls (153 `transfer`, 5 `transferFrom`, from
the Uniswap V2 pair and router) ⇒ 200/200 replayed, ZERO skips, zero
divergences**, against 188/200 with 12 closed-world skips without them. The
skips were an *artifact* of the missing calls: those transfers' balance
preconditions came from router traffic the replay never saw.

## Scope & constraints

**Sources.** Verified solc ^0.8.x, single-file *or* multi-file (real file tree
+ the verification's own remappings). A contract's own verified settings
(viaIR + optimizer) are used as a **fallback** when a default oracle compile
fails — modern stack-heavy contracts (Permit2) don't compile without them, and
making it a fallback keeps the existing corpus on the exact oracle it was
validated against. Pre-0.8 sources may opt into `--relax-pre08`: both legs then
compile the identical ^0.8-relaxed source, preserving differential-oracle
validity while explicitly giving up bytecode fidelity and pre-0.8 unchecked-
arithmetic fidelity. Any resulting historical-status flips remain closed-world
skips and must be reported separately.

**Architecturally out of reach** (no compiler work changes these):

- **Proxies.** Any EIP-1967 proxy is delegatecall, which cannot exist on the
  AVM — Lido, Compound III, most vaults. Measured over the top 40 protocols by
  TVL: 10 proxies, 11 pre-0.8, 13 viable.

  "Aave (all of it)" used to be on that list and was **wrong**: only its CORE
  is proxied (the Pool address resolves to
  `InitializableImmutableAdminUpgradeabilityProxy`). Its PERIPHERY is plain
  ^0.8.10 — `WrappedTokenGatewayV3`, `AaveOracle`, `PoolAddressesProvider`,
  `ACLManager`, `AaveProtocolDataProvider` are all non-proxy and verified, and
  the gateway replays. Worth re-probing a protocol per contract rather than
  writing off the whole name.
- **Vyper** (Curve, Yearn). Pre-0.8 Solidity is only source-differential via
  explicit pragma relaxation; it is not a byte-faithful replay of the deployed
  compiler's arithmetic semantics.
- **Unmodellable opcodes** — `codesize`, `extcodehash`/`.codehash` for a
  non-`this` address (its EVM value is a hash of EVM bytecode, meaningless
  across VMs), `tx.origin`. `extcodesize` is NOT among them: both it and
  `address(x).code` resolve the app id from the address's last 8 bytes — this
  compiler's contract-value convention — and read the real approval program via
  `app_params_get`, so `.code.length > 0` and OZ's assembly `Address.isContract`
  both answer correctly.
- **8 KB program cap** (1 base + 3 extra pages × 2048 B). Morpho Blue compiles
  at 13741 B and UniV4 PoolManager at 30751 B — both need the uros splitter to
  deploy. Permit2 (6218 B) fits and replays.
- **256 inner txns per top-level txn.** A Solidity loop of external calls
  (batch airdrop, multicall, liquidation sweep) has a ceiling the EVM lacks;
  verified on LocalNet at exactly 256 ok / 257 `too many inner transactions`.
  Classified as a platform limit so it can never masquerade as a finding.

**Replay-model limits:**

- **`msg.value` is replayed, SCALED identically on both legs** (default 1e12:
  1 ETH → 1 ALGO; `chd_common.VALUE_SCALE`, set 1 for a literal replay). A
  literal 1:1 is a supply impossibility, not a tuning choice: ETH carries 18
  decimals and ALGO 6, so the whole LocalNet supply (1.0e16 microAlgos) covers
  **5.8 median ETH transfers**, and only 3 of the corpus's 1580 value-bearing
  amounts are fundable literally.

  Applying the scale on BOTH legs is what keeps it sound. A *pass-through*
  contract behaves the same at any scale and replays for real; a
  *price-comparing* one (`require(msg.value >= price)`) sees a scaled
  msg.value against an unscaled price and reverts — **on both legs**, so the
  pair still agrees and the closed-world filter drops it. Scaling cannot
  manufacture a finding, only fail to gain coverage. Verified on friend.tech:
  342 value skips become real attempts that revert identically, case stays at
  zero divergences.

  Amounts exceeding uint64 after scaling are skipped as `value-too-wide`
  (24 of 1580) — an AVM amount field cannot express them.
- Outgoing external calls are not mocked; the closed-world filter skips any txn
  whose local result disagrees with its historical receipt, symmetrically.
- Constructors that call external contracts are only replayable when the
  dependency is itself verified, single-file and ^0.8 — otherwise the EVM
  *oracle* can't deploy either, and the case is skipped with that reason.
- Internal-call recovery needs a parent index, so today it covers **token**
  contracts. Traces are rate-limited (paced with retries) and any parent trace
  that can't be fetched is **reported**, never silently dropped.
- The window is the FIRST N txns from creation (state must be built from
  genesis). Internal calls consume that budget, so the window gets denser but
  shorter — and a contract whose internal traffic starts later than its first N
  txns gains nothing without a deeper window.
- Reverted historical txns are replayed and must revert on both legs (payload
  compared) — signal, not noise.
- **Block time is pinned on both legs, but the epoch may shift.** Both legs
  drive their clock to the same replayed instants, so `block.timestamp` agrees
  and every observable duration (cooldowns, vesting, permit expiry) is exact.
  The *absolute* epoch is only historical when LocalNet's clock still sits
  before the window; otherwise both legs replay the same deltas from a shared
  base just ahead of it. Two algod dev-mode facts force this, and both are the
  opposite of the obvious guess:

  - `global LatestTimestamp` reports the **previous** block's timestamp, so
    reaching an instant means sealing a block at it and *then* calling.
  - once a timestamp offset is set, dev mode computes each block as
    `previous + offset` and never consults the real clock again. The clock is
    therefore **strictly monotonic**: the offset endpoint is `uint64`, and
    setting 0 freezes time rather than re-zeroing it, so the AVM leg can never
    rewind to 2022 once it has been to 2026. Only restarting algod resets it.

  So the clock is a **ratchet**: each replay advances it by that window's whole
  historical span and it never comes back. `algokit localnet reset` rewinds it
  to 0 (verified) — and from 0 every historical epoch is reachable, so a reset
  before a batch is what buys true historical replay. The AVM leg deliberately
  does *not* restore the clock to wall clock afterwards: that would ratchet
  past the next case's window and force every later replay onto a shifted
  epoch. Drift beyond 30 days prints a warning, because a far-future epoch
  fails time-gated constructors — DEGEN deploys at 2026 and fails at 2028, and
  the case then drops out looking like an unrelated harness error.

  The shared base is re-derived per convergence attempt — the previous
  attempt's AVM run pushed the clock forward, and a base fixed once would
  silently disable pinning on every re-run after a platform-limit skip.

  Cost: one extra sealing block per distinct timestamp (58 for permit2's 66
  calls), since a call observes the *previous* block's time.

## Results

Every number below is a **two-leg** result: the same historical calldata, in
order, against py-evm (oracle) and the AVM, with state compared after each
snapshot. "Zero divergences" means no status, return-value, event, getter or
storage difference survived classification — it does NOT mean the whole window
executed: the closed-world filter drops transactions whose local result
disagrees with the historical receipt, and that count is reported per case.

Regenerate with `python3 results_table.py`. Each `cases/<tag>/report.json` is
that case's LAST run, so re-run a case in the mode you want to quote before
regenerating — a stale report reads exactly like a fresh one, which has bitten
this table twice (see below).

### Summary

| | |
|---|---|
| contracts replayed | **75** |
| zero divergences | **75** |
| with divergences | 0 |
| transactions replayed on both legs | **20,223** of 28,524 in-window (71%) |
| skipped, by cause | closed-world 7,101, avm-platform-limit 1,128, no-calldata 35, value 25, unknown-selector 12 |

### Headline runs

- **Deepest window** — `pepe_deep` replayed **1441/1500** transactions with zero
  divergences (1288 clock jumps), 7.2x the standard 200-transaction window.
- **Internal calls** — merging router-driven traffic took PEPE from 188/200 with
  12 closed-world skips to **200/200 with none**: the skips were an artifact of
  the missing calls, not of the compiler.
- **TVL-ranked slice** — SSV ($9.2B) 196/200, SDAO ($3.7B) 79/200, ether.fi
  ($3.2B) 33/200, Falcon ($1.3B) 70/200, Kelp ($0.9B) 58/200, Lighter ($0.5B)
  195/200, BMEX ($0.8B) 4/200 — all zero divergences. Selected by TVL, not by
  compiler-friendliness.
- **Permit2** — the first DeFi-infrastructure singleton under the program cap,
  66/200 clean and now with **no** timestamp values absorbed as noise.
- **CCTP v1 TokenMinter** — solc 0.7.6 source relaxed identically on both legs,
  full `--evm-layout`, O2 AVM size **6,432 B**. A selector-bounded parent set
  exercised **8 historical mints + 4 burns** through a taped USDC stand-in;
  **33/41 replayed**, 8 failed empty-calldata transactions skipped, zero
  pragma-induced status flips, zero divergences, platform limits, blind slots,
  or unattributed boxes. The burn-limit entry and 10 live composite-key token
  mappings were compared slot-for-slot.

### Open

Nothing in the corpus currently shows a divergence. What remains is structural:

- **8 KB program cap** — `pol` (8322 B, 130 B over), `strk`, `ens_tok`, `ondo`,
  `xtoken`, and both Polymarket contracts. Splitter territory, not compiler bugs.
- **Raft protocol** (RToken, PositionManager, ERC20Indexable, 0.8.19): the
  harness ladder works end-to-end — PriceFeed dep falls back to a stand-in,
  four tapes load, the missing-ctor ABI gets synthesized — and every contract
  then dies on ONE compile error: OZ `Address.functionDelegateCall`, vendored
  DEAD library code nothing ever calls (its only caller is its own overload).
  Same class as fbtc/gbp. **The unlock is reachability-gated delegatecall
  errors** (dead-function elimination before the hard error): five known
  contracts free on that single compiler change. Designed, not built.
- `wld` — constructor payload is 4276 B; the AVM caps create-txn app args at
  2048 B. Platform class (fix = bake ctor args into the program), counted.
- `blur` — 17-file multifile with `@openzeppelin` package imports; the EVM leg
  compiles it, the AVM leg's compile step lacks import remapping. Harness gap.
- `susde` — its constructor exhausts the oracle's whole 12 M gas budget
  (`gasUsed=12000000`): an out-of-gas, not a missing dependency, so
  `--stub-deps` has nothing to stand in for.
- `friendtech` in **slot mode**: **RESOLVED — zero divergences**, 43/400
  replayed, matching its default-model coverage exactly. The 39→10→0 chain was
  one real compiler bug (a payable function calling a non-payable public one
  reverted on its own payment — the guard lived in the shared method body, not
  the route) plus three harness bugs (a platform-limit classifier that matched
  algod's `opcodes=` disassembly field, a budget-pool group that overflowed
  MAX_GROUP_SIZE when a payment was attached, and inner-txn fee headroom that
  was only granted when constructor deps existed — friendtech's `buyShares`
  makes two fee payments via `.call{value}`, and the pool group came up exactly
  those two fees short).
- `fbtc`, `gbp` — **architectural, not bugs**: both use `delegatecall` (no AVM
  equivalent — shared-storage/caller-preservation semantics), and fbtc also
  uses `try`/`catch` (AVM has no in-transaction revert recovery). Both sit in
  vendored libraries, so they would only come back into reach if dead-path
  elimination ran before the hard errors.

### Per contract

| contract | chain | replayed | divergences | skips |
|---|---|---|---|---|
| `pepe_d3` PepeToken | ethereum | 2879/3000 | ✅ | closed-world 120, no-calldata 1 |
| `pepe_deep` PepeToken | ethereum | 1441/1500 | ✅ | closed-world 59 |
| `staup_d2` StauProject | polygon | 1014/2000 | ✅ | closed-world 734, avm-platform-limit 252 |
| `shfl_d` SHIA | ethereum | 990/1000 | ✅ | closed-world 10 |
| `floki_d` Bobo | ethereum | 945/1000 | ✅ | closed-world 55 |
| `usde_deep` USDe | ethereum | 865/1000 | ✅ | closed-world 135 |
| `wbt_deep` WBT | ethereum | 850/1000 | ✅ | closed-world 150 |
| `babydoge_d` FLOKI | ethereum | 825/1000 | ✅ | closed-world 175 |
| `staup_deep` StauProject | polygon | 772/1000 | ✅ | closed-world 228 |
| `bgb_d` BitgetToken | ethereum | 613/1000 | ✅ | closed-world 387 |
| `wbt_d2` WBT | ethereum | 590/2000 | ✅ | closed-world 956, avm-platform-limit 454 |
| `opmint` OptimismMintableERC20 | base | 285/300 | ✅ | closed-world 15 |
| `op_gov` GovernanceToken | optimism | 247/400 | ✅ | closed-world 100, avm-platform-limit 41, no-calldata 7, value 5 |
| `apecoin` Astgik | ethereum | 200/200 | ✅ | — |
| `l2custom` L2CustomERC20 | optimism | 200/200 | ✅ | — |
| `pepe_ic` PepeToken | ethereum | 200/200 | ✅ | — |
| `staup` StauProject | polygon | 200/200 | ✅ | — |
| `wbt` WBT | ethereum | 200/200 | ✅ | — |
| `bobo` Bobo | ethereum | 199/200 | ✅ | closed-world 1 |
| `opmint9` OptimismMintableERC20 | base | 199/200 | ✅ | closed-world 1 |
| `aero_base` Aero | base | 198/200 | ✅ | closed-world 2 |
| `aero` Aero | base | 197/200 | ✅ | closed-world 2, avm-platform-limit 1 |
| `burnminterc20` BurnMintERC20 | arbitrum | 197/200 | ✅ | closed-world 3 |
| `turbo` Turbo | ethereum | 197/200 | ✅ | closed-world 3 |
| `frxeth` frxETH | ethereum | 196/200 | ✅ | closed-world 4 |
| `kizuna` WojakCoin | ethereum | 196/200 | ✅ | closed-world 4 |
| `ssv` SSVToken | ethereum | 196/200 | ✅ | closed-world 4 |
| `ladys` LadysToken | ethereum | 195/200 | ✅ | closed-world 5 |
| `lighter` Lighter | ethereum | 195/200 | ✅ | closed-world 2, no-calldata 1, avm-platform-limit 1, value 1 |
| `venice` Venice | base | 195/200 | ✅ | closed-world 5 |
| `velo` Velo | optimism | 193/200 | ✅ | closed-world 7 |
| `xvs` XVS | optimism | 192/200 | ✅ | closed-world 8 |
| `higher` Token | base | 191/200 | ✅ | closed-world 8, value 1 |
| `shfl` SHFL | ethereum | 191/200 | ✅ | closed-world 9 |
| `degen` DegenToken | base | 189/200 | ✅ | closed-world 11 |
| `temple` TempleERC20Token | ethereum | 189/200 | ✅ | closed-world 10, value 1 |
| `doginme` doginme | base | 188/200 | ✅ | closed-world 12 |
| `floki` FLOKI | ethereum | 188/200 | ✅ | closed-world 12 |
| `pepe` PepeToken | ethereum | 188/200 | ✅ | closed-world 12 |
| `wallettok` WALLETToken | polygon | 188/200 | ✅ | closed-world 12 |
| `systemcoin` SystemCoin | optimism | 186/200 | ✅ | closed-world 14 |
| `extra` EXTRA | optimism | 182/200 | ✅ | closed-world 16, avm-platform-limit 2 |
| `usde` USDe | ethereum | 162/200 | ✅ | closed-world 38 |
| `babydoge` BabyDogeToken | base | 154/200 | ✅ | closed-world 45, avm-platform-limit 1 |
| `kaito` Kaito | base | 153/200 | ✅ | closed-world 42, avm-platform-limit 3, value 2 |
| `erc6160` ERC6160Ext20 | ethereum | 150/200 | ✅ | closed-world 50 |
| `aave_gateway` WrappedTokenGatewayV3 | ethereum | 148/200 | ✅ | closed-world 52 |
| `ape` Astgik | ethereum | 131/200 | ✅ | closed-world 66, value 3 |
| `gho` GhoToken | ethereum | 119/200 | ✅ | closed-world 81 |
| `bgb` BitgetToken | ethereum | 115/200 | ✅ | closed-world 84, avm-platform-limit 1 |
| `ena` ENA | ethereum | 111/200 | ✅ | closed-world 86, no-calldata 3 |
| `pika` Pika | optimism | 100/200 | ✅ | closed-world 83, avm-platform-limit 17 |
| `apepe` APEPE | polygon | 98/200 | ✅ | closed-world 102 |
| `imx` IMXToken | ethereum | 85/200 | ✅ | closed-world 112, value 2, avm-platform-limit 1 |
| `pbnj` PBNJ | gnosis | 85/200 | ✅ | closed-world 114, avm-platform-limit 1 |
| `zen` ZenToken | base | 84/200 | ✅ | closed-world 115, avm-platform-limit 1 |
| `sdao` SDAO | ethereum | 79/200 | ✅ | closed-world 120, value 1 |
| `sand` PolygonSand | polygon | 78/200 | ✅ | closed-world 122 |
| `permit2` Permit2 | ethereum | 74/200 | ✅ | closed-world 101, no-calldata 13, unknown-selector 12 |
| `falcon` FF | ethereum | 70/200 | ✅ | closed-world 130 |
| `kelp` KERNEL | ethereum | 58/200 | ✅ | closed-world 141, avm-platform-limit 1 |
| `xerc20` XERC20 | gnosis | 44/200 | ✅ | closed-world 156 |
| `friendtech` FriendtechSharesV1 | base | 43/400 | ✅ | closed-world 357 |
| `strk` StarkNetToken | ethereum | 42/400 | ✅ | avm-platform-limit 349, value 7, no-calldata 2 |
| `cctp_minter` TokenMinter | ethereum | 33/41 | ✅ | no-calldata 8 |
| `etherfi` EtherFiGovernanceToken | ethereum | 33/200 | ✅ | closed-world 163, value 2, avm-platform-limit 2 |
| `selftest` StorageShapes | synthetic | 10/10 | ✅ | — |
| `vanry` VANRY | polygon | 9/200 | ✅ | closed-world 191 |
| `bmex` BMEX | ethereum | 5/200 | ✅ | closed-world 195 |
| `morpho_alloc` PublicAllocator | ethereum | 3/200 | ✅ | closed-world 197 |
| `cow_ethflow` CoWSwapEthFlow | ethereum | 2/200 | ✅ | closed-world 198 |
| `tig` TIGToken | base | 2/200 | ✅ | closed-world 198 |
| `comet_rewards` CometRewards | ethereum | 1/200 | ✅ | closed-world 199 |
| `sqd` SQD | arbitrum | 1/200 | ✅ | closed-world 199 |
| `aave_oracle` AaveOracle | ethereum | 0/73 | ✅ | closed-world 73 |

### Aave (Ethereum)

| contract | oracle | AVM |
|---|---|---|
| `WrappedTokenGatewayV3` | ✅ | ✅ **replays 148/200, zero divergences** — the self-address stub lets its Pool calls answer, so the ETH-wrapping (msg.value) path replays end-to-end |
| `PoolAddressesProvider` | ✅ replays | ❌ architectural — it deploys proxies via `new`, so `delegatecall` + `extcodehash` |
| `ACLManager` | ❌ ctor calls the provider (a stand-in ERC-20 has no `getACLAdmin()`) | — |
| Pool, aTokens, debt tokens | — | EIP-1967 proxies, out of reach |

The gateway's 1/200 is not a compiler result: **153 of its transactions carry
ETH value** and the replay model skips those (`msg.value == 0` only), which is
inherent to a contract whose whole job is wrapping ETH. It found a real
compiler bug on the way in — `switch returndatasize()` in Gnosis
`GPv2SafeERC20` (vendored by Aave *and* CoW) hit a Yul-switch type mismatch.

### What these numbers do not cover

The corpus is what the constraints above admit, and the selection is visibly
biased by them: no proxies (so no Aave/Lido/Compound), only opt-in source-level
coverage for pre-0.8, and nothing over the 8 KB program cap without the splitter.
Coverage per case varies by two
orders of magnitude (1/200 to 1441/1500) and is dominated by how much of a
contract's traffic depends on external state we cannot reconstruct — deploying
a dependency supplies its code, not its history.

## Corpus status

~40 real contracts replay their on-chain history with **zero divergences** in
slot mode, spanning plain ERC-20s, permit/ShortStrings, ERC20Votes/Checkpoints,
tax-on-transfer, pausable/role-gated, bridged L2 tokens, ERC-4626-adjacent
vault tokens — and **Permit2**, the first DeFi-infrastructure singleton that
fits under the program cap.

Candidate selection matters: holder-ranked harvesting yields memecoins and
bridged tokens, while `harvest.py --tvl` yields staking / restaking / lending
governance tokens, which are far heavier users of checkpoints, permits and
role machinery. Of the 13 viable contracts in the top-40-by-TVL slice, 10/10
attempted so far **compile** in slot+memory mode and 8 fit under the cap
(3.8–7.9 KB) — a rate worth noting because none were chosen for
compiler-friendliness.

Every divergence this campaign surfaced was triaged to one of: a real compiler
bug (fixed — see the mode's design notes), a platform limit, or an
environment-noise class documented above.

## Reading the report

`cases/<tag>/report.json` + console summary. Categories:
- `status_div` — one leg reverted, the other didn't (after closed-world
  filtering this is REAL signal).
- `value_div` — both succeeded, return values differ.
- `event_div` — emitted events differ (count, name, or args).
- `snapshot_div` — zero-arg getter state drifted between legs.
- `storage_div` / `storage_map_div` — state differs by Solidity variable name.
  Mapping divergences carry `last_write_txn`, the last txn that wrote that map,
  so the cause is localised instead of just "the end states differ".
- `skips` — per-reason counts (value / no-calldata / unknown-selector /
  closed-world / avm-platform-limit / unmapped-sender).

### Noise classes (reported, not counted as findings)

Some values *must* differ between two local chains and would diverge for a
perfect compiler. These are classified, with the reason attached, rather than
whitelisted away wholesale:

- **chain id** — `DOMAIN_SEPARATOR()`, `chainId()`, and `eip712Domain()`, whose
  field 3 is the chain id (compared **masked**: the other six fields still have
  to match exactly, so a real divergence there is still reported).
- **block height** — `clock()` (ERC-6372).
- **timestamps** — largely eliminated by clock pinning (below). What remains is
  py-evm advancing its own clock one second per mined block, which drifts
  against the AVM whenever several calls share a historical second. Applied to
  scalars, getter snapshots, *and element-wise inside struct/array map values*
  (Permit2 fills `PackedAllowance.expiration` with `now`), gated on both values
  being plausible unix times within 300 s — measured agreement across permit2's
  window was `[-5, +1]` s. A genuinely wrong field (0, a counter, a hash) is
  never absorbed. This window was **7 days** before pinning, wide enough to
  swallow a real bug.
- **uniform offsets** — every entry of a map differing by the same delta.

Coverage warnings (⚠️) matter as much as divergences here: a comparison that
never happened reports as zero divergences, which looks exactly like a pass.

- `storage_maps_uncompared` — declared by the contract, not diffed.
- `storage_maps_unavailable` — EVM found mapping state, AVM found none.
- `storage_blind_slots` — slots the EVM leg *saw written* that no probe reads.
- `storage_boxes_unattributed` — boxes on chain that no derived key matched.

## Storage tracing

There is no `debug_traceTransaction` on eth-tester, but the EVM runs
**in-process**, so every SSTORE funnels through `AccountDB.set_storage` and
patching that one method gives an exact per-txn written-slot set — cheaper and
more robust than decoding an opcode stream. Two things fall out of it:

1. **Localisation** — a mapping divergence names the txn that last wrote it.
2. **Honest coverage** — every traced slot is checked against the set of slots
   the readers actually looked at. A slot written but never read is state the
   differ is *blind* to, and it says so rather than counting it clean.

### Mapping shapes the readers cover

Both legs derive candidate keys **forward** (the hash is one-way but never
needs inverting) and stay O(txns) — never a cartesian product over symbols:

| shape | key source |
|---|---|
| `mapping(address => V)` | every registry symbol |
| `mapping(address => mapping(address => V))` | sender ↔ address-arg pairs the replay actually made |
| `mapping(address => mapping(address => mapping(address => V)))` | (sender, arg_i, arg_j) triples — Permit2 `allowance` |
| `mapping(address => mapping(uint => V))` | small word indices + each uint arg's `>> 8` — Permit2 `nonceBitmap` |
| `mapping(bytes32 => V)` | bytes32 args seen in calls (OZ AccessControl roles) |

Values decode as scalar, struct (per-member) or dynamic array. Extending this
is how a *vacuous* pass becomes a real one: Permit2 first replayed "clean" with
`allowance` and `nonceBitmap` reported **uncompared** and 50 blind slots —
adding depth-3 and uint-inner-key support immediately surfaced three real
entries to compare (which then proved to be timestamp skew).

The AVM leg mirrors (2) by enumerating the app's real boxes and reporting any
that no forward-derived key matched. That is the only check that can catch a
**wrong key derivation**: get the hash wrong and both legs find nothing for a
mapping, which is indistinguishable from a mapping that is genuinely empty.

Caveat, stated plainly: a write inside a frame that later reverts is journalled
away by py-evm but still appears in the trace, so the trace over-approximates.
That is the safe direction — it can over-state a blind spot, never hide one.
