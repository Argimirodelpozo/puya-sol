# chainwide-historical-diff

Replay the **real historical transaction sequence** of a deployed (verified)
contract against two local legs, in lockstep from contract creation, and diff
them:

- **EVM leg** — local py-evm re-execution (eth-tester), multi-sender, real
  constructor args, per-txn historical timestamps. This is the **oracle**.
- **AVM leg** — the same source compiled by puya-sol, deployed on LocalNet,
  driven through the same decoded call sequence with the same (mapped) senders.

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

# self-test the STORAGE DIFFER on a synthetic contract (no network)
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python selftest.py
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
fetch.py     Blockscout (keyless): verified source + ABI + constructor args +
             ASCENDING txn history (old Etherscan-compat API, sort=asc).
             → cases/<tag>/{case.json, source.sol, prepared.sol}
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

## v1 scope (deliberate)

- Single-file verified sources, solc ^0.8.x (both legs compile the same
  `prepared.sol`; exact-pinned pragmas are relaxed to `^`).
- `msg.value == 0` txns only (value-bearing txns are skipped symmetrically;
  wei↔microAlgo is unit-incompatible).
- Outgoing external calls: not mocked yet — the closed-world filter skips any
  txn whose local result disagrees with the historical receipt. (v2: recorded
  call traces or real-Solidity dependency mocks; see session notes.)
- Reverted historical txns are replayed and must revert on both legs (payload
  compared) — they are signal, not noise.
- Time pinning: EVM leg pins block timestamps to historical values;
  AVM-side dev-mode offset pinning is TODO — time-derived divergences are a
  known-noise class meanwhile.

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

The AVM leg mirrors (2) by enumerating the app's real boxes and reporting any
that no forward-derived key matched. That is the only check that can catch a
**wrong key derivation**: get the hash wrong and both legs find nothing for a
mapping, which is indistinguishable from a mapping that is genuinely empty.

Caveat, stated plainly: a write inside a frame that later reverts is journalled
away by py-evm but still appears in the trace, so the trace over-approximates.
That is the safe direction — it can over-state a blind spot, never hide one.
