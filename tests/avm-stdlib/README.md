# `tests/avm-stdlib/` — Solidity ↔ AVM stdlib regression suite

End-to-end tests for the Solidity-callable AVM standard library
exposed at [`src/libs/AVM.sol`](../../src/libs/AVM.sol), imported by user
contracts as `libs/AVM.sol`.

Each test compiles a small fixture that imports a subset of the
stdlib's libraries (`AVM`, `Crypto`, `Group`, `Txn`, `Global`), deploys
to localnet, and asserts the runtime opcodes do what they claim.

## Why a separate dir?

The semantic-test suite under
[`tests/solidity-semantic-tests/`](../solidity-semantic-tests/) is
imported from the upstream Solidity test corpus and tests Solidity
language features against EVM expectations. The AVM stdlib is a
*new* surface that doesn't have an EVM analogue — it exists to give
Solidity contracts access to Algorand-native primitives (ASAs, Falcon,
group transaction inspection, etc.). Keeping its tests outside the
semantic suite avoids polluting the EVM-divergence diff with
intentionally AVM-only behavior.

## Files

| File | Purpose |
|---|---|
| `conftest.py`               | Reuses the semantic-test `Harness` fixture; adds `tests/solidity-semantic-tests/` to `sys.path` so the `framework` module imports cleanly |
| `contracts/txn_global.sol`  | `Txn.sender / fee / applicationId / numAppArgs / typeEnum`, `Global.currentApplicationId / Address / round / opcodeBudget / latestTimestamp`, `Group.size / index` |
| `contracts/crypto_group.sol`| `Crypto.sha512_256 / sha3_256 / ed25519Verify / falconVerify`, `Group.txnSender / txnReceiver / txnAmount / txnType / txnFee` |
| `contracts/asa_lifecycle.sol` | `AVM.asaCreate / asaOptIn / asaTransfer / asaBalance / asaTotalSupply / asaDestroy` round-trip |
| `test_avm_stdlib.py`        | pytest cases — one per primitive |

## Running

```bash
# From repo root, with localnet running:
PUYASOL_LOCALNET_RESET=0 python3 -m pytest -p no:cacheprovider tests/avm-stdlib/ -v
```

xdist works: `-n auto` runs tests in parallel under separate output dirs.

## Adding a new intrinsic

1. Add the Solidity stub to [`src/libs/AVM.sol`](../../src/libs/AVM.sol)
   under the right library (`AVM`, `Crypto`, `Group`, `Txn`, `Global`).
   Body should revert as a safety net — the compiler intercept replaces
   the call before the body runs.
2. Add a dispatch handler in
   [`src/builder/itxn/AsaIntrinsics.cpp`](../../src/builder/itxn/AsaIntrinsics.cpp)
   under the matching `dispatchX` (or `handleAsaXxx` for ASA).
3. Add a test fixture + test here. Reuse the existing
   `==== Source: AVM.sol ====` + `==== Source: contract.sol ====`
   pattern so fixtures are self-contained.

## Coverage limits

The Falcon test exercises the wrong-public-key-size revert path, not a valid
Falcon-512 signature. A successful run therefore does not establish that full
verification path. Use the current pytest output for test counts; the removed
result snapshot was a historical run, not a current baseline.
