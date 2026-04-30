"""Translation of v2 src/test/CollateralToken.t.sol — all 32 tests.

CollateralToken is a UUPS-upgradeable ERC20 (pUSD, 6 decimals) wrapping
USDC/USDCe. The Foundry test pulls in `CollateralSetup._deploy(owner)`
which builds the full stack (USDC + USDCe + vault + token + onramp/offramp/
permissioned-ramp). On AVM we wire the same set up via fixtures:
`usdc`, `usdce`, `vault`, `collateral_token`, plus a
`MockCollateralTokenRouter` deployment for the wrap/unwrap callback paths.

The original blocker (`PUYA_ABI_ARG_DROP` — apparent ABI-router
missing-`dup` for `initialize(address)`) was fixed in
`puya-sol: fix account ensureBiguint + __postInit ensure_budget hook`
[55fd3e233]. Real cause was upstream: `AssemblyBuilder::ensureBiguint`
silently coerced `account` values to `IntegerConstant(0)`, so Solady's
`or(newOwner, shl(255, iszero(newOwner)))` compiled to a constant.
"""
from pathlib import Path

import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, PaymentTxn, StateSchema,
    wait_for_confirmation,
)

from dev.addrs import addr, algod_addr_for_app, app_id_to_address
from dev.arc56 import compile_teal, inject_memory_init, load_arc56
from dev.deploy import AUTO_POPULATE, NO_POPULATE, create_app, deploy_app
from dev.invoke import call


# wrap / unwrap go through Solady's `SafeTransferLib.safeTransfer` (and
# `safeTransferFrom`) on the asset address. Solady writes those as inline
# assembly that does `call(gas, token, 0, ptr, len, retPtr, retLen)` on a
# non-constant `token`. puya-sol's Yul `call` handler currently stubs the
# call as success without firing the corresponding `itxn ApplicationCall`,
# so the transfer silently no-ops on AVM (see TransferHelper.sol's
# AVM-PORT-ADAPTATION note + PUYA_BLOCKERS.md §3). The pUSD mint/burn half
# of wrap/unwrap fires correctly; the asset side does not move. Until
# puya-sol learns to lower SafeTransferLib's call-with-non-constant-target,
# these flows can only be exercised end-to-end if the source switches to
# the IERC20Min wrapper (which TransferHelper already uses for its own
# ERC20 paths).
SAFETRANSFERLIB_CALL_STUB = (
    "Solady SafeTransferLib emits inline-assembly `call(gas, token, …)` "
    "on a non-constant token; puya-sol stubs that call as success without "
    "firing an inner txn, so the asset transfer silently no-ops. "
    "PUYA_BLOCKERS.md §3."
)

# AVM-PORT-ADAPTATION: the with-callback unwrap path is structurally
# blocked by AVM's no-reentrancy rule. ct.unwrap fires a callback to
# router.unwrapCallback, which tries to pull pUSD from the funder via
# `IERC20Min(collateralToken).transferFrom(...)`. Since `collateralToken`
# IS ct (ct is its own ERC20 surface), this is a re-entrant call into
# ct from inside ct's own call frame. EVM permits this; AVM forbids
# it (`attempt to re-enter X`). The wrap-with-callback variant works
# because the callback target there is the asset (USDC), a different
# app from ct. The no-callback unwrap variant works for the same reason
# (no callback chain). Fixing this would require splitting ct's ERC20
# surface into a sibling app, which is out of scope for the audited
# port.
UNWRAP_CALLBACK_REENTRY = (
    "ct.unwrap → router.unwrapCallback → ct.transferFrom would re-enter "
    "ct, which AVM forbids. Structural difference between EVM (allows "
    "reentrancy) and AVM (forbids it). The no-callback unwrap variant "
    "covers the same wrap/unwrap semantics; the only thing this test "
    "specifically exercises that the no-callback can't is the callback "
    "dispatch chain — which works for `wrap` (callback target is USDC, "
    "a different app) but is unreachable for `unwrap`."
)


# `collateral_token_wired` (real USDC/USDCe/vault) is defined in
# test/conftest.py so it's reachable from sibling onramp/offramp tests.


# ── INITIALIZE ────────────────────────────────────────────────────────────


def test_CollateralToken_initialize(collateral_token, admin):
    """initialize(admin) sets owner() == admin."""
    assert call(collateral_token, "owner") == admin.address


def test_revert_CollateralToken_initialize_alreadyInitialized(collateral_token, admin):
    """A second `initialize(_)` call reverts with InvalidInitialization."""
    with pytest.raises(LogicError):
        call(collateral_token, "initialize", [addr(admin)])


# ── VIEW (pure, no owner needed) ─────────────────────────────────────────


def test_CollateralToken_name(collateral_token):
    """name() returns the constant `Polymarket USD`."""
    assert call(collateral_token, "name") == "Polymarket USD"


def test_CollateralToken_symbol(collateral_token):
    """symbol() returns `pUSD`."""
    assert call(collateral_token, "symbol") == "pUSD"


def test_CollateralToken_decimals(collateral_token):
    """decimals() returns 6 (USDC parity)."""
    assert call(collateral_token, "decimals") == 6


def test_CollateralToken_immutables(collateral_token_wired, usdc_stateful, usdce_stateful, vault):
    """USDC/USDCE/VAULT immutables match what was passed to __postInit."""
    assert call(collateral_token_wired, "USDC") == encoding.encode_address(
        app_id_to_address(usdc_stateful.app_id))
    assert call(collateral_token_wired, "USDCE") == encoding.encode_address(
        app_id_to_address(usdce_stateful.app_id))
    assert call(collateral_token_wired, "VAULT") == vault.address


# ── ROLE MANAGEMENT (positive paths need owner) ──────────────────────────

MINTER_ROLE = 1 << 0
WRAPPER_ROLE = 1 << 1


def test_CollateralToken_addMinter(collateral_token, admin, funded_account):
    """addMinter as owner grants MINTER_ROLE."""
    call(collateral_token, "addMinter", [addr(funded_account)], sender=admin)
    assert call(collateral_token, "hasAllRoles",
                [addr(funded_account), MINTER_ROLE]) is True


def test_CollateralToken_removeMinter(collateral_token, admin, funded_account):
    """removeMinter as owner revokes MINTER_ROLE."""
    call(collateral_token, "addMinter", [addr(funded_account)], sender=admin)
    assert call(collateral_token, "hasAllRoles",
                [addr(funded_account), MINTER_ROLE]) is True
    call(collateral_token, "removeMinter", [addr(funded_account)], sender=admin)
    assert call(collateral_token, "hasAllRoles",
                [addr(funded_account), MINTER_ROLE]) is False


def test_CollateralToken_addWrapper(collateral_token, admin, funded_account):
    """addWrapper as owner grants WRAPPER_ROLE."""
    call(collateral_token, "addWrapper", [addr(funded_account)], sender=admin)
    assert call(collateral_token, "hasAllRoles",
                [addr(funded_account), WRAPPER_ROLE]) is True


def test_CollateralToken_removeWrapper(collateral_token, admin, funded_account):
    """removeWrapper as owner revokes WRAPPER_ROLE."""
    call(collateral_token, "addWrapper", [addr(funded_account)], sender=admin)
    call(collateral_token, "removeWrapper", [addr(funded_account)], sender=admin)
    assert call(collateral_token, "hasAllRoles",
                [addr(funded_account), WRAPPER_ROLE]) is False


# ── ROLE MANAGEMENT (negative paths — no owner dependency) ──────────────


def test_revert_CollateralToken_addMinter_unauthorized(collateral_token, funded_account):
    """addMinter from a non-owner reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "addMinter", [addr(funded_account)], sender=funded_account)


def test_revert_CollateralToken_removeMinter_unauthorized(collateral_token, funded_account):
    """removeMinter from a non-owner reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "removeMinter", [addr(funded_account)], sender=funded_account)


def test_revert_CollateralToken_addWrapper_unauthorized(collateral_token, funded_account):
    """addWrapper from a non-owner reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "addWrapper", [addr(funded_account)], sender=funded_account)


def test_revert_CollateralToken_removeWrapper_unauthorized(collateral_token, funded_account):
    """removeWrapper from a non-owner reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "removeWrapper", [addr(funded_account)], sender=funded_account)


# ── MINT / BURN ──────────────────────────────────────────────────────────


def test_CollateralToken_mint(collateral_token, admin, funded_account):
    """A minter can mint pUSD to a recipient."""
    call(collateral_token, "addMinter", [addr(admin)], sender=admin)
    call(collateral_token, "mint", [addr(funded_account), 100_000_000], sender=admin)
    assert call(collateral_token, "balanceOf", [addr(funded_account)]) == 100_000_000


def test_revert_CollateralToken_mint_unauthorized(collateral_token, funded_account):
    """mint from a non-minter reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "mint",
             [addr(funded_account), 100_000_000],
             sender=funded_account)


def test_CollateralToken_burn(collateral_token, admin):
    """A minter can burn its own pUSD."""
    call(collateral_token, "addMinter", [addr(admin)], sender=admin)
    call(collateral_token, "mint", [addr(admin), 100_000_000], sender=admin)
    assert call(collateral_token, "balanceOf", [addr(admin)]) == 100_000_000
    call(collateral_token, "burn", [100_000_000], sender=admin)
    assert call(collateral_token, "balanceOf", [addr(admin)]) == 0


def test_revert_CollateralToken_burn_unauthorized(collateral_token, funded_account):
    """burn from a non-minter reverts."""
    with pytest.raises(LogicError):
        call(collateral_token, "burn", [100_000_000], sender=funded_account)


# ── WRAP (with callback) ─────────────────────────────────────────────────


def _deploy_router(localnet, admin, collateral_token_app_id):
    """Deploy a MockCollateralTokenRouter pointed at the collateral token
    under test. Mirrors `new MockCollateralTokenRouter(address(collateral.token))`
    from the Foundry suite.

    AVM-PORT-ADAPTATION: the router takes two views of ct's address —
    psol-encoded for inner-tx targeting (so puya-sol's `extract_uint64`
    at offset 24 finds the app id) and real-Algorand for recipient
    args (so the asset receiver-mock credits the same `balances[…]`
    key that ct's later `usdc.transfer(VAULT, …)` reads from with
    `msg.sender = ct_real`). See router source for the AVM-PORT
    rationale.
    """
    from dev.addrs import algod_addr_bytes_for_app
    out_dir = Path(__file__).parent.parent.parent / "out"
    base = out_dir / "test" / "dev" / "mocks"
    return deploy_app(
        localnet, admin, base, "MockCollateralTokenRouter",
        create_args=[
            app_id_to_address(collateral_token_app_id),     # psol-encoded
            algod_addr_bytes_for_app(collateral_token_app_id),  # real Algorand
        ],
    )


def _wrap_with_callback(localnet, collateral_token_wired, asset_mock,
                        vault, admin, recipient, amount=100_000_000):
    """Wrap exercising the callback path. The caller (admin/funder)
    pre-funds the asset to themselves and approves the router; the
    router's `wrapCallback` does the actual transferFrom into
    collateral_token.

    AVM-PORT-ADAPTATION: in the Foundry version, the test calls
    `router.wrap(...)` which internally calls `ct.wrap(_, _, _, address(this), data)`.
    On AVM, `address(this)` lowers to `global CurrentApplicationAddress`
    (the real 32-byte Algorand address of the router), not puya-sol's
    Solidity-side storage-encoding (`\\x00*24 || itob(app_id)`). When ct
    later fires `ICollateralTokenCallbacks(_callbackReceiver).wrapCallback(...)`,
    it `extract_uint64`s from offset 24 to recover the inner-tx's
    ApplicationID — which only works against the storage-encoding. So
    we skip the router's wrap helper and call ct.wrap directly with the
    router's storage-encoded address as `_callbackReceiver`. The
    router's `wrapCallback` body is the same logic that Foundry's
    `router.wrap` would have driven; the wrapper sugar layer is the
    only thing we drop.

    Mirrors Foundry's:
        usdc.mint(funder, amt); usdc.approve(router, amt);
        ct.addWrapper(funder); router.wrap(usdc, recipient, amt);
    """
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    from dev.addrs import algod_addr_bytes_for_app

    router = _deploy_router(localnet, admin, collateral_token_wired.app_id)
    router_addr32 = algod_addr_bytes_for_app(router.app_id)

    # Owner grants WRAPPER_ROLE to admin (the direct caller of ct.wrap).
    # Role storage keys against admin's real Algorand address.
    call(collateral_token_wired, "addWrapper",
         [addr(admin)], sender=admin)

    # Funder mints the asset, then approves the router as a spender. The
    # router's wrapCallback fires usdc.transferFrom(funder, ct, amt) and
    # needs an allowance from `funder` for that pull to succeed.
    funder32 = addr(admin)
    deal_usdc(asset_mock, funder32, amount)
    set_allowance(asset_mock, funder32, router_addr32, amount)

    ct_addr32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    vault_before = usdc_balance(asset_mock, addr(vault))
    funder_before = usdc_balance(asset_mock, funder32)

    # data = abi.encode(funder). For a single address arg, EVM abi.encode
    # produces just the 32-byte left-padded address — the same shape as
    # an Algorand 32-byte address bytes value. Pass the raw bytes.
    data = funder32

    call(
        collateral_token_wired, "wrap",
        [
            app_id_to_address(asset_mock.app_id),
            addr(recipient),
            amount,
            app_id_to_address(router.app_id),  # _callbackReceiver = router (storage-encoded)
            data,
        ],
        sender=admin,
        app_references=[router.app_id, asset_mock.app_id],
    )

    # pUSD minted to recipient.
    assert call(collateral_token_wired, "balanceOf",
                [addr(recipient)]) == amount
    # Asset went funder → ct → vault. Net: funder's balance dropped by
    # amount, vault's grew by amount, ct's net is zero (received in the
    # callback and forwarded to vault in the same call).
    assert usdc_balance(asset_mock, funder32) == funder_before - amount
    assert usdc_balance(asset_mock, ct_addr32) == 0
    assert usdc_balance(asset_mock, addr(vault)) == vault_before + amount


def test_CollateralToken_wrapUSDC(
    localnet, collateral_token_wired, usdc_stateful, vault, admin, funded_account
):
    """Wrap USDC via the callback router. Should mint pUSD to recipient,
    transfer USDC to the vault."""
    _wrap_with_callback(localnet, collateral_token_wired, usdc_stateful,
                        vault, admin, funded_account)


def test_CollateralToken_wrapUSDCe(
    localnet, collateral_token_wired, usdce_stateful, vault, admin, funded_account
):
    """Wrap USDCe via the callback router."""
    _wrap_with_callback(localnet, collateral_token_wired, usdce_stateful,
                        vault, admin, funded_account)


# ── WRAP (without callback) ─────────────────────────────────────────────


def _wrap_noCallback(collateral_token_wired, asset_mock, vault, admin,
                     recipient, amount=100_000_000):
    """Common no-callback wrap fixture body. The caller deposits the asset
    into the collateral token's app account ahead of time; `wrap` then
    mints pUSD to `recipient` and transfers the asset to `vault`."""
    from dev.deals import deal_usdc, usdc_balance
    from dev.addrs import algod_addr_bytes_for_app

    # Owner grants WRAPPER_ROLE to admin so admin can call wrap.
    call(collateral_token_wired, "addWrapper", [addr(admin)], sender=admin)

    # Pre-deposit the asset into collateral_token's app account. When
    # collateral_token inner-calls usdc.transfer(VAULT, …), the mock sees
    # `op.Txn.sender == collateral_token's account`, so the balance must
    # exist under THAT key — algod_addr_bytes_for_app gives us those raw
    # 32 bytes. (`app_id_to_address` returns puya-sol's Solidity-side
    # convention `\x00*24 + itob(appid)`, which is what gets stored in
    # the contract's `address` slots — different value, different table.)
    ct_addr32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    deal_usdc(asset_mock, ct_addr32, amount)
    assert usdc_balance(asset_mock, ct_addr32) == amount

    # Snapshot vault balance before — it might be non-zero from earlier
    # fund_random_account top-up (vault is just a funded account).
    vault_before = usdc_balance(asset_mock, addr(vault))

    call(
        collateral_token_wired, "wrap",
        [
            app_id_to_address(asset_mock.app_id),
            addr(recipient),
            amount,
            b"\x00" * 32,  # callbackReceiver = address(0) → no callback
            b"",            # data
        ],
        sender=admin,
    )

    # pUSD minted to recipient.
    assert call(collateral_token_wired, "balanceOf", [addr(recipient)]) == amount
    # Asset moved out of collateral_token, into vault.
    assert usdc_balance(asset_mock, ct_addr32) == 0
    assert usdc_balance(asset_mock, addr(vault)) == vault_before + amount


def test_CollateralToken_wrapUSDC_noCallback(
    collateral_token_wired, usdc_stateful, vault, admin, funded_account
):
    """No-callback variant: caller pre-deposits USDC to the token, then
    `wrap` mints pUSD to recipient and transfers USDC to the vault."""
    _wrap_noCallback(collateral_token_wired, usdc_stateful, vault, admin, funded_account)


def test_CollateralToken_wrapUSDCe_noCallback(
    collateral_token_wired, usdce_stateful, vault, admin, funded_account
):
    """No-callback wrap of USDCe — same flow as USDC, just the other
    supported asset slot."""
    _wrap_noCallback(collateral_token_wired, usdce_stateful, vault, admin, funded_account)


# ── WRAP (revert paths) ─────────────────────────────────────────────────


def test_revert_CollateralToken_wrap_unauthorized(collateral_token_wired, usdc, funded_account):
    """wrap from a non-wrapper reverts (Ownable.Unauthorized)."""
    with pytest.raises(LogicError):
        call(
            collateral_token_wired, "wrap",
            [
                app_id_to_address(usdc.app_id),
                addr(funded_account),
                100_000_000,
                b"\x00" * 32,  # callbackReceiver = address(0)
                b"",            # data
            ],
            sender=funded_account,
        )


def test_revert_CollateralToken_wrapInvalidAsset(collateral_token_wired, funded_account):
    """wrap with an asset that's neither USDC nor USDCE reverts (InvalidAsset)."""
    # An arbitrary "invalid" asset — we use the funded_account's address
    # as a stand-in, since wrap will compare it to the (real) USDC/USDCE
    # immutables and find neither matches.
    with pytest.raises(LogicError):
        call(
            collateral_token_wired, "wrap",
            [
                addr(funded_account),  # _invalidAsset
                addr(funded_account),
                100_000_000,
                b"\x00" * 32,
                b"",
            ],
            sender=funded_account,
        )


# ── UNWRAP (with callback) ──────────────────────────────────────────────


def _unwrap_with_callback(localnet, collateral_token_wired, asset_mock,
                          vault, admin, recipient, amount=100_000_000):
    """Unwrap exercising the callback path.

    AVM-PORT-ADAPTATION: same `address(this)` storage-encoding caveat as
    `_wrap_with_callback` — we skip the router's `unwrap` sugar and call
    ct.unwrap directly with the router's storage-encoded address as
    `_callbackReceiver`.
    """
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    from dev.addrs import algod_addr_bytes_for_app

    router = _deploy_router(localnet, admin, collateral_token_wired.app_id)
    router_addr32 = algod_addr_bytes_for_app(router.app_id)

    # Owner grants roles. Admin gets MINTER (to pre-mint pUSD) and
    # WRAPPER (to call ct.unwrap directly). Storage keys to admin's
    # real Algorand address.
    call(collateral_token_wired, "addMinter",
         [addr(admin)], sender=admin)
    call(collateral_token_wired, "addWrapper",
         [addr(admin)], sender=admin)

    # Mint the funder some pUSD (admin is the funder). Funder approves
    # the router on the collateral_token's own ERC20 ABI so the router's
    # unwrapCallback can pull pUSD from funder into ct.
    funder32 = addr(admin)
    call(collateral_token_wired, "mint",
         [funder32, amount], sender=admin)
    call(
        collateral_token_wired, "approve",
        [router_addr32, amount],
        sender=admin,
    )

    # Vault holds the underlying asset and approves ct to pull on its
    # behalf — ct.unwrap fires `IERC20Min(_asset).transferFrom(VAULT, _to, _amount)`.
    deal_usdc(asset_mock, addr(vault), amount)
    set_allowance(asset_mock, addr(vault),
                  algod_addr_bytes_for_app(collateral_token_wired.app_id),
                  amount)

    ct_addr32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    recipient_before = usdc_balance(asset_mock, addr(recipient))

    # data = abi.encode(funder).
    from algosdk import abi as algo_abi
    data = algo_abi.ABIType.from_string("(address)").encode((admin.address,))

    call(
        collateral_token_wired, "unwrap",
        [
            app_id_to_address(asset_mock.app_id),
            addr(recipient),
            amount,
            app_id_to_address(router.app_id),  # _callbackReceiver = router
            data,
        ],
        sender=admin,
        app_references=[router.app_id, asset_mock.app_id],
    )

    # pUSD net: funder had `amount`, transferred to ct via router, ct
    # burned. Funder's balance is 0; ct's balance is 0.
    assert call(collateral_token_wired, "balanceOf", [funder32]) == 0
    assert call(collateral_token_wired, "balanceOf", [ct_addr32]) == 0
    # Asset went vault → recipient.
    assert usdc_balance(asset_mock, addr(vault)) == 0
    assert usdc_balance(asset_mock, addr(recipient)) == recipient_before + amount


@pytest.mark.xfail(reason=UNWRAP_CALLBACK_REENTRY, strict=False)
def test_CollateralToken_unwrapUSDC(
    localnet, collateral_token_wired, usdc_stateful, vault, admin, funded_account
):
    """Unwrap USDC via the callback router."""
    _unwrap_with_callback(localnet, collateral_token_wired, usdc_stateful,
                          vault, admin, funded_account)


@pytest.mark.xfail(reason=UNWRAP_CALLBACK_REENTRY, strict=False)
def test_CollateralToken_unwrapUSDCe(
    localnet, collateral_token_wired, usdce_stateful, vault, admin, funded_account
):
    """Unwrap USDCe via the callback router."""
    _unwrap_with_callback(localnet, collateral_token_wired, usdce_stateful,
                          vault, admin, funded_account)


# ── UNWRAP (without callback) ───────────────────────────────────────────


def test_CollateralToken_unwrapUSDC_noCallback(
    collateral_token_wired, usdc_stateful, vault, admin, funded_account
):
    """No-callback variant: caller pre-transfers pUSD to the token, then
    unwrap() pulls USDC from vault to recipient and burns the pUSD that
    was already deposited on the contract."""
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    from dev.addrs import algod_addr_bytes_for_app

    asset_mock = usdc_stateful
    amount = 100_000_000
    recipient = funded_account

    # admin gets MINTER so we can mint pUSD and pre-deposit it on ct.
    call(collateral_token_wired, "addMinter", [addr(admin)], sender=admin)
    call(collateral_token_wired, "addWrapper", [addr(admin)], sender=admin)

    # Mint pUSD straight onto ct's own balance — that's what unwrap
    # will burn from `address(this)`.
    ct_addr32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    call(collateral_token_wired, "mint", [ct_addr32, amount], sender=admin)
    assert call(collateral_token_wired, "balanceOf", [ct_addr32]) == amount

    # Vault holds the asset and approves ct to pull it.
    deal_usdc(asset_mock, addr(vault), amount)
    set_allowance(asset_mock, addr(vault), ct_addr32, amount)

    recipient_before = usdc_balance(asset_mock, addr(recipient))

    call(
        collateral_token_wired, "unwrap",
        [
            app_id_to_address(asset_mock.app_id),
            addr(recipient),
            amount,
            b"\x00" * 32,  # callbackReceiver = address(0) → no callback
            b"",            # data
        ],
        sender=admin,
    )

    # pUSD on ct burned.
    assert call(collateral_token_wired, "balanceOf", [ct_addr32]) == 0
    # Asset moved vault → recipient.
    assert usdc_balance(asset_mock, addr(vault)) == 0
    assert usdc_balance(asset_mock, addr(recipient)) == recipient_before + amount


# ── UNWRAP (revert paths) ───────────────────────────────────────────────


def test_revert_CollateralToken_unwrap_unauthorized(collateral_token_wired, usdc, funded_account):
    """unwrap from a non-wrapper reverts."""
    with pytest.raises(LogicError):
        call(
            collateral_token_wired, "unwrap",
            [
                app_id_to_address(usdc.app_id),
                addr(funded_account),
                100_000_000,
                b"\x00" * 32,
                b"",
            ],
            sender=funded_account,
        )


def test_revert_CollateralToken_unwrapInvalidAsset(collateral_token_wired, funded_account):
    """unwrap with an asset not in {USDC, USDCE} reverts (InvalidAsset)."""
    with pytest.raises(LogicError):
        call(
            collateral_token_wired, "unwrap",
            [
                addr(funded_account),  # _invalidAsset
                addr(funded_account),
                100_000_000,
                b"\x00" * 32,
                b"",
            ],
            sender=funded_account,
        )


# ── PERMIT2 ─────────────────────────────────────────────────────────────


def test_CollateralToken_permit2NoInfiniteAllowance(collateral_token, admin):
    """Solady's Permit2 infinite-allowance bypass is disabled — allowance
    to the canonical Permit2 reads back as 0 even for `admin`."""
    permit2_eth_addr = bytes.fromhex("000000000022D473030F116dDEE9F6B43aC78BA3")
    permit2_avm_addr = b"\x00" * 12 + permit2_eth_addr  # left-pad to 32 bytes
    assert call(
        collateral_token, "allowance",
        [addr(admin), permit2_avm_addr],
    ) == 0


# ── UUPS UPGRADE ────────────────────────────────────────────────────────


def test_CollateralToken_upgradeToAndCall(collateral_token, admin, usdc, usdce, vault):
    """Owner can upgradeToAndCall(newImpl, "").

    AVM-PORT-ADAPTATION: CollateralToken overrides Solady's
    `upgradeToAndCall` (whose body is inline-asm with hardcoded EVM
    selectors and a delegatecall puya-sol can't lower) with a
    high-level Solidity version that gates on `_authorizeUpgrade`
    and emits `Upgraded`. Sufficient for the test's intent: owner
    can call, non-owner is rejected (separate revert test)."""
    # Deploying a second CollateralToken instance to act as "newImpl".
    # In production this would be a fully-deployed app; for the test we
    # just need an address that the upgrade can install.
    # See collateral_token_wired for the AVM-port flat-layout note.
    OUT_DIR = Path(__file__).parent.parent.parent / "out"
    base = OUT_DIR / "collateral" / "CollateralToken"
    algod = collateral_token.algorand.client.algod

    orch_spec = load_arc56(base / "CollateralToken.arc56.json")
    orch_teal = (base / "CollateralToken.approval.teal").read_text()
    orch_clear = (base / "CollateralToken.clear.teal").read_text()
    sch = orch_spec.state.schema.global_state
    approval_bin = compile_teal(algod, orch_teal)
    clear_bin = compile_teal(algod, orch_clear)
    extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
    new_impl_app_id = create_app(
        collateral_token.algorand, admin,
        approval_bin, clear_bin,
        sch, extra_pages=extra_pages,
        app_args=[b"\x00" * 32, b"\x00" * 32, b"\x00" * 32],
    )
    call(
        collateral_token, "upgradeToAndCall",
        [app_id_to_address(new_impl_app_id), b""],
        sender=admin,
    )


def test_revert_CollateralToken_upgradeToAndCall_unauthorized(collateral_token, funded_account):
    """upgradeToAndCall from a non-owner reverts (UUPS _authorizeUpgrade)."""
    with pytest.raises(LogicError):
        call(
            collateral_token, "upgradeToAndCall",
            [addr(funded_account), b""],
            sender=funded_account,
        )
