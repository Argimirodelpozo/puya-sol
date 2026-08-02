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
python3 replay.py usde --evm-layout                 # storage mode
python3 replay.py morpho --evm-layout --evm-memory  # + memory mode
python3 batch.py --evm-layout --max-txns 200 --only usde,degen

# INTERNAL (contract-to-contract) CALLS merged into the stream — for a
# router-traded token this is most of the real traffic (see below).
python3 fetch.py eth.blockscout.com 0x6982...1933 pepe --max-txns 200 --internal

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
             whitelist (e.g. DOMAIN_SEPARATOR()).
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
  byte-identical calldata and the same `msg.sender` symbol. (Constructor
  *dependencies* are different — those are fetched, compiled and deployed on
  both legs, because the contract really does call out during construction.)

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
validated against.

**Architecturally out of reach** (no compiler work changes these):

- **Proxies.** Any EIP-1967 proxy is delegatecall, which cannot exist on the
  AVM — Aave (all of it), Lido, Compound III, most vaults. Measured over the
  top 40 protocols by TVL: 10 proxies, 11 pre-0.8, 13 viable.
- **Pre-0.8 Solidity**, and Vyper (Curve, Yearn).
- **Unmodellable opcodes** — `codesize`/`extcodesize` (no program-length
  introspection), `tx.origin`. (`address(x).code` *is* supported: the app id is
  resolved from the address's last 8 bytes, this compiler's contract-value
  convention, and `app_params_get` returns the real approval program — so
  `.code.length > 0` guards answer correctly.)
- **8 KB program cap** (1 base + 3 extra pages × 2048 B). Morpho Blue compiles
  at 13741 B and UniV4 PoolManager at 30751 B — both need the uros splitter to
  deploy. Permit2 (6218 B) fits and replays.
- **256 inner txns per top-level txn.** A Solidity loop of external calls
  (batch airdrop, multicall, liquidation sweep) has a ceiling the EVM lacks;
  verified on LocalNet at exactly 256 ok / 257 `too many inner transactions`.
  Classified as a platform limit so it can never masquerade as a finding.

**Replay-model limits:**

- `msg.value == 0` txns only (wei↔microAlgo is unit-incompatible).
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
- **AVM block time is not pinned.** The EVM leg time-travels to each txn's
  historical timestamp; the AVM leg runs at LocalNet wall clock. Time-derived
  values are classified as noise (below); algod dev-mode offset pinning is the
  real fix and would convert some closed-world skips into coverage.

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
- **timestamps** — a value set from `block.timestamp` differs because the EVM
  leg time-travels while the AVM leg runs at wall clock. Applied to scalars,
  getter snapshots, *and element-wise inside struct/array map values* (Permit2
  fills `PackedAllowance.expiration` with `now`), with a plausible-unix-time +
  2 h-window test so it can only absorb clock skew.
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
