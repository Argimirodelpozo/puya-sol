# AVM Port Adaptations

This document tracks every source-level deviation between the audited
EVM v2 codebase and the AVM-deployable variant in this directory.
Every change is tagged in source with a `// AVM-PORT-ADAPTATION` comment
and falls into one of the categories below.

The guiding principle: keep the audited Solidity untouched wherever
possible; only adapt the spots where a Solidity-or-EVM-specific construct
doesn't translate to AVM via puya-sol. Each adaptation includes the root
cause, the fix shape, and (where applicable) which test exercises it.

---

## 1. Solady SafeTransferLib → IERC20Min interface calls

**Why:** Solady's `SafeTransferLib` emits inline-assembly
`call(gas, token, 0, ptr, len, retPtr, retLen)` against a non-constant
`token` address. puya-sol's Yul `call` handler currently stubs that
call as success without firing the corresponding `itxn ApplicationCall`,
so the transfer/approve silently no-ops on AVM. (See
`PUYA_BLOCKERS.md §3` for the upstream context.)

**Fix shape:** define a minimal interface and invoke through it. Plain
`IERC20.transfer(...)` / `transferFrom(...)` / `approve(...)` /
`balanceOf(...)` lowers to a normal inner-app-call in the orch's TEAL.
The pattern matches what `TransferHelper.sol` already used:

```solidity
interface IERC20Min {
    function approve(address spender, uint256 amount) external returns (bool);
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}
```

Every `using SafeTransferLib for address;` is dropped, every
`x.safeTransfer(...)` / `safeTransferFrom(...)` / `safeApprove(...)`
becomes `require(IERC20Min(x).<fn>(...), "ERC20 ... failed")`, and
`x.balanceOf(...)` becomes `IERC20Min(x).balanceOf(...)`.

**Files touched:**
- `src/collateral/CollateralToken.sol`
- `src/test/dev/mocks/MockCollateralTokenRouter.sol`
- `src/adapters/CtfCollateralAdapter.sol`
- `src/adapters/NegRiskCtfCollateralAdapter.sol` (re-uses
  `IERC20Min` from `CtfCollateralAdapter.sol`)

**Tests unblocked:** all CollateralToken `wrap`/`unwrap` no-callback
variants, the `wrapInvalidAsset`/`unwrap_unauthorized` revert paths,
the `immutables` checks; CtfCollateralAdapter / NegRisk
adapter splitPosition/mergePositions/redeemPositions paths.

---

## 2. Solady UUPSUpgradeable.upgradeToAndCall override

**Why:** `UUPSUpgradeable.upgradeToAndCall` is implemented as inline-asm
that does:
1. a hardcoded EVM-keccak `proxiableUUID()` selector check via
   `staticcall` to `newImplementation`,
2. `log2` for `Upgraded(address)`,
3. `sstore` of the implementation slot,
4. an optional `delegatecall` into `newImplementation` for
   initialization data.

None of those lower correctly:
- `staticcall` to a non-constant target is stubbed as success returning
  zero, so the slot-equality check always fails with `UpgradeFailed()`;
- `log2` is unsupported (see §4 below);
- `sstore` to a magic 1967 slot has no AVM analog;
- `delegatecall` is stubbed.

The `onlyProxy` modifier is also vacuously true on AVM, since the
"implementation contract" *is* the only contract in this universe — no
proxy → implementation address comparison applies.

**Fix shape:** override `upgradeToAndCall` in `CollateralToken.sol`
with a high-level Solidity body that gates on `_authorizeUpgrade` and
emits the `Upgraded` event. The signature stays identical for ABI
compatibility, the `data` calldata payload is discarded.

```solidity
function upgradeToAndCall(address newImplementation, bytes calldata) public payable override {
    _authorizeUpgrade(newImplementation);
    emit Upgraded(newImplementation);
}
```

**File touched:** `src/collateral/CollateralToken.sol`

**Tests unblocked:** `test_CollateralToken_upgradeToAndCall`.

---

## 3. Custom AVM-port admin functions in CTFExchange

Three admin-only methods bridge AVM-side identity/storage gaps that
have no EVM analog. All three follow the `_avmPort*` naming convention,
all three are `onlyAdmin` and idempotent. They live in
`src/exchange/CTFExchange.sol`.

### 3a. `_avmPortGrantCtfOperator(address operator)`

**Why:** the splitter peels TransferHelper out into a sibling helper1
contract. That changes the effective `msg.sender` on inner CTF calls
from `address(this)` (the exchange) to helper1's app account. CTFMock's
`isApprovedForAll` check therefore fails on every orch→user
CTF transfer (the MINT/MERGE distribute path) because helper1 was
never granted `setApprovalForAll`. The audited `Assets` constructor
only approves `outcomeTokenFactory`.

**Fix shape:** call `ERC1155(getCtf()).setApprovalForAll(operator, true)`
from an admin entrypoint so the deploy harness can grant helper1 the
same blanket approval at __postInit time.

### 3b. `_avmPortApproveCollateralSpender(address spender)`

**Why:** `Assets`'s constructor approves the `outcomeTokenFactory`
keyed against the puya-sol storage-slot encoding of that address
(`\x00*24 + itob(app_id)`). When CTFMock later calls
`usdc.transferFrom(orch, ctf, _amount)`, the receiver checks
allowance against the **real** Algorand address of the caller (32-byte
sha512_256-derived account address). The two encodings disagree, so
the lookup misses.

**Fix shape:** the deploy harness calls this with the receiver-side
real address so the keys line up.

### 3c. `_avmPortPauseUser(address user)`

**Why:** `pauseUser()` writes `userPausedBlockAt[msg.sender]`. Test
identities `bob` / `carla` are eth-style `Account.from_key(0xB0B)` /
`Account.from_key(0xCA414)` EOAs — they have no Algorand private key,
so they can never be `op.Txn.sender`. Foundry's `vm.prank(bob);
ct.pauseUser()` has no AVM analog.

**Fix shape:** an admin cheat that writes the same storage slot
`pauseUser()` does, but takes the target as an argument.

**Tests unblocked:** matchOrders MINT/MERGE settlement (§3a, §3b);
`test_match_orders_preapproved_respects_user_pause` (§3c).

---

## 4. Inline-asm event emission → Solidity `emit`

**Why:** v2's `Events.sol` originally emitted events through inline-asm
`log2`/`log3`/`log4` with hardcoded EVM-keccak topic constants
(`_ORDER_FILLED_TOPIC` etc.). puya-sol's Yul handler does not lower
the `log*` family to AVM `op.log`, so the events fired in the EVM but
emitted nothing on AVM.

**Fix shape:** declare `event OrderFilled(...)`, `event OrdersMatched(...)`,
`event FeeCharged(...)` at the contract level and replace each
inline-asm block with a plain `emit Foo(...)`. puya-sol then generates
ARC-28-shape `op.log` payloads (`selector(4) ++ arc4_encode(args)`).
Notes on the wire shape:
- `bytes32` and `address` lower to `uint8[32]` in the ARC-28 selector
  signature.
- Narrow ints (`uint8`, etc.) widen to `uint64` on the wire.

These fully replace the EVM topic-based shape — AVM `op.log` is
data-only (no topic concept).

**File touched:** `src/exchange/mixins/Events.sol`

**Tests unblocked:** `test_match_orders_events_complementary_with_fees`.

---

## 5. Inline-asm helpers in Trading + AssetOperations

**Why:** two helper functions in `src/exchange/mixins/Trading.sol` and
`src/exchange/mixins/AssetOperations.sol` were written as inline-asm
loops / array constructions. puya-sol's AssemblyBuilder doesn't
translate the specific patterns (loop counters reaching back into
calldata, `mstore` against a memory base computed from `add`).

**Fix shape:** rewrite each in plain Solidity. Behavior is identical;
gas is slightly higher, but irrelevant on AVM.

- `Trading._isAllComplementary(...)`: replaced inline-asm scan with a
  for-loop over `makerOrders[i].side != takerSide`.
- `AssetOperations._getPartition()`: replaced inline-asm
  `mstore(arr, 1); mstore(arr, 2)` with `new uint256[](2); arr[0] = 1;
  arr[1] = 2;`.

---

## 6. MockCollateralTokenRouter relative-import inlining

**Why:** the audited Foundry mock imports
`ICollateralTokenCallbacks` via
`../../../collateral/interfaces/ICollateralTokenCallbacks.sol`. puya-sol's
import resolver does not allow upward traversal through `../` past the
source file's parent dir, so this relative path doesn't resolve.

**Fix shape:** inline the callback interface in
`MockCollateralTokenRouter.sol` itself. The signatures must match
`CollateralToken.sol`'s callback callsites exactly.

---

## 7. Test-mock UniversalMock surface extension

**Why:** the `UniversalMock` test contract is a no-op stand-in for
"any token-ish contract the v2 exchange constructor touches during
deploy." NegRiskCtfCollateralAdapter's constructor calls
`INegRiskAdapter(_negRiskAdapter).wcol()` to read the wrapped-collateral
address, which UniversalMock didn't expose.

**Fix shape:** add `wcol()` returning `address(this)` to the mock so
the constructor's inner-call resolves.

**File touched:** `src/test/dev/mocks/UniversalMock.sol`

---

## 8. CollateralToken artifact-path layout (test-side)

**Why:** before §1 was applied to CollateralToken, puya-sol's splitter
auto-extracted `SafeTransferLib.safeTransfer` into a
`CallContextChecker__Helper1` sidecar, producing artifacts under
`out/collateral/CollateralToken/{CallContextChecker__Helper1,
CollateralToken}/...`. After §1, no name in the splitter's fallback
list matches CollateralToken's AWST, so extraction is skipped and the
output goes flat: `out/collateral/CollateralToken/CollateralToken.{teal,
arc56.json,bin}`.

**Fix shape:** the `collateral_token` / `collateral_token_wired`
fixtures (in `test/conftest.py` and
`test/collateral/test_collateral_token.py`) read from the flat layout.

---

## 9. Test-side fixture: `usdce_stateful` for wrap/unwrap selectors

**Why:** the Solidity USDC.sol mock inherits Solady ERC20, whose
`transfer(address,uint256)` lowers to ARC-28 `transfer(address,uint512)bool`
(selector `0x42820278`). The AVM-port IERC20Min uses
`transfer(address,uint256)bool` (selector `0x198c9820`). Selector
mismatch → the inner-tx fires successfully but the receiver's match
table errors out.

**Fix shape:** the wrap/unwrap tests use the Python delegate
`USDCMock` (`delegate/usdc_mock.py`), whose `transfer(address,uint256)bool`
selector matches IERC20Min. The conftest provides `usdc_stateful`
already; this adaptation adds a sibling `usdce_stateful` fixture that
deploys a second USDCMock instance (different app id, same selectors).
The Solidity `usdc`/`usdce` fixtures stay available for tests that
don't exercise actual transfer semantics.

**File touched:** `test/conftest.py`,
`test/collateral/test_collateral_token.py`

---

## Address-encoding split: psol vs algod-real

The two views of an app's address are at the heart of several
adaptations:
- **psol-encoded** (`\x00*24 || itob(app_id)`): how puya-sol stores
  Solidity-typed `address` values in slots and how it expects to
  resolve inner-tx ApplicationIDs (it does `extract_uint64` at
  offset 24 to recover the app id).
- **algod-real** (`sha512_256("appID" || app_id)`): the actual on-
  chain account address. This is what `op.Txn.sender` resolves to
  inside an inner-call (the immediately-calling app's account),
  what `usdc.balances[…]` and `usdc.allowances[…]` are keyed on,
  and what `global CurrentApplicationAddress` returns (i.e. what
  `address(this)` lowers to).

EVM collapses both into a single 20-byte address. AVM doesn't —
they're two different 32-byte values. Most of the AVM-port-
adaptation friction is reconciling which one is needed where.

Pattern: a contract that wants to act both as
- an inner-tx target (needs psol so puya-sol can extract the app id)
  and
- a recipient/owner of asset state in another contract (needs algod-
  real so the receiver mock's `balances[<key>]` lookup matches what
  later inner calls use)

…must store **both** addresses. See §6/§9 below for two examples
where this came up.

## Known-bad / xfailed AVM-port issues

These are tracked as `pytest.mark.xfail` or `pytest.mark.skip` because
the root cause is structural (AVM vs EVM semantics) or remains
under investigation. Each entry points at the smallest concrete repro.

### A. Re-entrant unwrap callback

**Symptom:** `ct.unwrap` fires `router.unwrapCallback`, which itself
fires `IERC20Min(collateralToken).transferFrom(...)` — a re-entrant
call into ct, since `collateralToken` *is* ct (ct is its own pUSD
ERC20 surface). EVM permits this; AVM forbids it (`attempt to
re-enter X`).

**Tests affected:** `test_CollateralToken_unwrap{USDC,USDCe}` (the
with-callback variants only). The no-callback variants pass.

**Possible fix (out of scope for the audited port):** split ct's
ERC20 surface into a sibling app so the unwrap callback doesn't
re-enter ct.

### B. ToggleableERC1271Mock fixture wiring (1271 happy path)

(Resolved.) Translated as
`test_match_orders_preapproved_1271_signer_invalidated`. See test
file for the full flow.

### C. NegRiskAdapter wcol() target

(Resolved.) Was a fixture-level mismatch — the test used
`mock_token` (a v1 ERC20 mock with 14 methods, no `wcol()`) for the
`_negRiskAdapter` slot, so the constructor's
`INegRiskAdapter(_negRiskAdapter).wcol()` inner-tx reached an app
whose method table didn't match the selector. Switched the
`negrisk_adapter` fixture to `universal_mock` (which exposes a
no-op `wcol()` returning `address(this)`), and the deploy succeeds.
Not a compiler issue at all.

---

## Summary table

| #   | Adaptation                                | Files                                                          |
| --- | ----------------------------------------- | -------------------------------------------------------------- |
| 1   | SafeTransferLib → IERC20Min               | CollateralToken.sol, MockCollateralTokenRouter.sol, both adapters |
| 2   | UUPSUpgradeable.upgradeToAndCall override | CollateralToken.sol                                            |
| 3a  | `_avmPortGrantCtfOperator`                | CTFExchange.sol                                                |
| 3b  | `_avmPortApproveCollateralSpender`        | CTFExchange.sol                                                |
| 3c  | `_avmPortPauseUser`                       | CTFExchange.sol                                                |
| 4   | Inline-asm log* → Solidity `emit`         | Events.sol                                                     |
| 5   | Inline-asm helpers → high-level Solidity  | Trading.sol (`_isAllComplementary`), AssetOperations.sol (`_getPartition`) |
| 6   | Inline ICollateralTokenCallbacks          | MockCollateralTokenRouter.sol                                  |
| 7   | UniversalMock surface extension           | UniversalMock.sol (`wcol()`)                                   |
| 8   | Flat artifact layout                      | (test-side path constants only)                                |
| 9   | `usdce_stateful` fixture                  | (test-side fixture only)                                       |
