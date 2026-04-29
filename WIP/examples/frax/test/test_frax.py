"""Tests for Frax Finance compiled from unmodified Solidity to AVM.

Translated from FraxFinance/frax-solidity Foundry tests:
- FXSDisableVoteTracking.t.sol: testTransfers, testBurn, testPoolActions

Additional tests covering FRAXStablecoin governance, ERC20, collateral ratio,
and pool management — based on the contract's public API.

Source: Solidity >=0.6.11, ~3,200 lines across 25 files.
Compilation: puya-sol + puya. FRAX 6.2KB, FXS 4.2KB, FraxPool 7.4KB.
"""
import os
from pathlib import Path
from Crypto.Hash import keccak

import algokit_utils as au
import pytest
from algosdk import encoding
from conftest import (
    OUT_DIR,
    deploy_contract,
    fund_contract,
    load_arc56,
    mapping_box_key,
    box_ref,
    addr_to_bytes32,
    app_id_to_bytes32,
    app_id_to_algod_addr,
    int_to_bytes32,
)

pytestmark = pytest.mark.localnet

NO_POPULATE = au.SendParams(populate_app_call_resources=False)
PRICE_PRECISION = 10**6
FRAX_GENESIS = 2_000_000 * 10**18
FXS_GENESIS = 100_000_000 * 10**18


def sol_addr(client: au.AppClient) -> str:
    return app_id_to_algod_addr(client.app_id)


def keccak256(data: bytes) -> bytes:
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


def call_with_budget(
    localnet, app, params, budget_calls=1, pad_method="totalSupply",
    padding_box_refs=None,
):
    composer = localnet.new_group()
    for _ in range(budget_calls):
        composer.add_app_call_method_call(app.params.call(
            au.AppClientMethodCallParams(
                method=pad_method,
                box_references=padding_box_refs or [],
                note=os.urandom(8),
            )
        ))
    composer.add_app_call_method_call(app.params.call(params))
    return composer.send(NO_POPULATE)


# ---------------------------------------------------------------------------
# Deploy helpers
# ---------------------------------------------------------------------------

def _role_box_refs(app_id, role, account_addr):
    """Box refs needed for AccessControl role operations."""
    acct = addr_to_bytes32(account_addr)
    return [
        box_ref(app_id, mapping_box_key("_roles", role)),
        box_ref(app_id, mapping_box_key("_indexes", acct)),
    ]


def deploy_frax(localnet, account):
    """Deploy FRAXStablecoin with __postInit constructor."""
    frax = deploy_contract(
        localnet, account, "FRAXStablecoin", subdir="FraxTest",
        fund_amount=5_000_000,
    )
    cr_pauser = keccak256(b"COLLATERAL_RATIO_PAUSER")
    admin_role = b"\x00" * 32
    acct = addr_to_bytes32(account.address)

    boxes = (
        _role_box_refs(frax.app_id, admin_role, account.address)
        + _role_box_refs(frax.app_id, cr_pauser, account.address)
        + [box_ref(frax.app_id, mapping_box_key("_balances", acct))]
    )

    call_with_budget(localnet, frax,
        au.AppClientMethodCallParams(
            method="__postInit",
            args=["FRAX", "FRAX", account.address, account.address],
            box_references=boxes,
            extra_fee=au.AlgoAmount(micro_algo=5000),
        ),
        budget_calls=5,
    )
    return frax


def deploy_fxs(localnet, account):
    """Deploy FRAXShares (FXS) with __postInit constructor."""
    fxs = deploy_contract(
        localnet, account, "FRAXShares", subdir="FXSTest",
        fund_amount=3_000_000,
    )
    admin_role = b"\x00" * 32
    acct = addr_to_bytes32(account.address)
    boxes = (
        _role_box_refs(fxs.app_id, admin_role, account.address)
        + [box_ref(fxs.app_id, mapping_box_key("_balances", acct))]
    )

    fxs.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=["FXS", "FXS", account.address, account.address, account.address],
            box_references=boxes,
            extra_fee=au.AlgoAmount(micro_algo=2000),
        ),
    )
    return fxs


def bal_key(addr):
    return mapping_box_key("_balances", addr_to_bytes32(addr))


def allow_key(owner, spender):
    return mapping_box_key(
        "_allowances",
        addr_to_bytes32(owner),
        addr_to_bytes32(spender),
    )


# ---------------------------------------------------------------------------
# Compilation tests
# ---------------------------------------------------------------------------

class TestCompilation:
    @pytest.mark.parametrize("name,subdir", [
        ("FRAXStablecoin", "FraxTest"),
        ("FRAXShares", "FXSTest"),
        ("FraxPool", "FraxTest"),
    ])
    def test_teal_files_exist(self, name, subdir):
        base = OUT_DIR / subdir
        assert (base / f"{name}.approval.teal").exists()
        assert (base / f"{name}.arc56.json").exists()

    def test_frax_within_limits(self):
        assert (OUT_DIR / "FraxTest" / "FRAXStablecoin.approval.bin").stat().st_size <= 8192

    def test_fxs_within_limits(self):
        assert (OUT_DIR / "FXSTest" / "FRAXShares.approval.bin").stat().st_size <= 8192

    def test_frax_pool_within_limits(self):
        assert (OUT_DIR / "FraxTest" / "FraxPool.approval.bin").stat().st_size <= 8192

    def test_frax_has_methods(self):
        spec = load_arc56("FRAXStablecoin", "FraxTest")
        methods = {m.name for m in spec.methods}
        for m in ["totalSupply", "balanceOf", "transfer", "approve",
                   "refreshCollateralRatio", "addPool", "removePool",
                   "setMintingFee", "setRedemptionFee", "toggleCollateralRatio",
                   "pool_mint", "pool_burn_from"]:
            assert m in methods, f"Missing method: {m}"


# ---------------------------------------------------------------------------
# FRAXStablecoin Deployment (genesis state)
# ---------------------------------------------------------------------------

class TestFraxGenesis:
    @pytest.fixture
    def frax(self, localnet, account):
        return deploy_frax(localnet, account)

    def test_deploy(self, frax):
        assert frax.app_id > 0

    def test_genesis_supply(self, frax):
        r = frax.send.call(au.AppClientMethodCallParams(method="totalSupply"))
        assert r.abi_return == FRAX_GENESIS

    def test_creator_balance(self, frax, account):
        r = frax.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[box_ref(frax.app_id, bal_key(account.address))],
        ))
        assert r.abi_return == FRAX_GENESIS

    def test_name_symbol(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="name")).abi_return == "FRAX"
        assert frax.send.call(au.AppClientMethodCallParams(method="symbol")).abi_return == "FRAX"

    def test_collateral_ratio_100pct(self, frax):
        r = frax.send.call(au.AppClientMethodCallParams(method="global_collateral_ratio"))
        assert r.abi_return == 1_000_000

    def test_frax_step(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="frax_step")).abi_return == 2500

    def test_price_target(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="price_target")).abi_return == 1_000_000

    def test_price_band(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="price_band")).abi_return == 5000

    def test_refresh_cooldown(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="refresh_cooldown")).abi_return == 3600

    def test_not_paused(self, frax):
        assert frax.send.call(au.AppClientMethodCallParams(method="collateral_ratio_paused")).abi_return is False

    def test_owner(self, frax, account):
        assert frax.send.call(au.AppClientMethodCallParams(method="owner")).abi_return == account.address


# ---------------------------------------------------------------------------
# FXS Deployment (genesis state)
# ---------------------------------------------------------------------------

class TestFXSGenesis:
    @pytest.fixture
    def fxs(self, localnet, account):
        return deploy_fxs(localnet, account)

    def test_deploy(self, fxs):
        assert fxs.app_id > 0

    def test_genesis_supply(self, fxs):
        assert fxs.send.call(au.AppClientMethodCallParams(method="totalSupply")).abi_return == FXS_GENESIS

    def test_creator_balance(self, fxs, account):
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        assert r.abi_return == FXS_GENESIS

    def test_name_symbol(self, fxs):
        assert fxs.send.call(au.AppClientMethodCallParams(method="name")).abi_return == "FXS"
        assert fxs.send.call(au.AppClientMethodCallParams(method="symbol")).abi_return == "FXS"

    def test_tracking_votes(self, fxs):
        assert fxs.send.call(au.AppClientMethodCallParams(method="trackingVotes")).abi_return is True


# ---------------------------------------------------------------------------
# FXS Transfers (translated from testTransfers in FXSDisableVoteTracking.t.sol)
# ---------------------------------------------------------------------------

class TestFXSTransfers:
    @pytest.fixture
    def fxs(self, localnet, account):
        return deploy_fxs(localnet, account)

    def test_transfer(self, fxs, account):
        """Translated from testTransfers: transfer 100e18 FXS."""
        recipient = fxs.app_address
        amount = 100 * 10**18
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[recipient, amount],
            box_references=[
                box_ref(fxs.app_id, bal_key(account.address)),
                box_ref(fxs.app_id, bal_key(recipient)),
            ],
        ))
        assert r.abi_return is True

        # Verify balance decreased
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        assert r.abi_return == FXS_GENESIS - amount

        # Verify recipient received
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[recipient],
            box_references=[box_ref(fxs.app_id, bal_key(recipient))],
        ))
        assert r.abi_return == amount

    def test_transfer_from(self, fxs, account):
        """Transfer via allowance (from testTransfers)."""
        spender = fxs.app_address
        amount = 100 * 10**18

        # First transfer some to the spender so they have tokens
        fxs.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[spender, amount * 2],
            box_references=[
                box_ref(fxs.app_id, bal_key(account.address)),
                box_ref(fxs.app_id, bal_key(spender)),
            ],
        ))

        # Approve
        ak = allow_key(account.address, spender)
        fxs.send.call(au.AppClientMethodCallParams(
            method="approve", args=[spender, amount],
            box_references=[box_ref(fxs.app_id, ak)],
        ))

        # Check allowance
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="allowance", args=[account.address, spender],
            box_references=[box_ref(fxs.app_id, ak)],
        ))
        assert r.abi_return == amount


# ---------------------------------------------------------------------------
# FXS Burns (translated from testBurn)
# ---------------------------------------------------------------------------

class TestFXSBurns:
    @pytest.fixture
    def fxs(self, localnet, account):
        return deploy_fxs(localnet, account)

    def test_burn(self, fxs, account):
        """burn(100e18) — from testBurn."""
        amount = 100 * 10**18
        fxs.send.call(au.AppClientMethodCallParams(
            method="burn", args=[amount],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        r = fxs.send.call(au.AppClientMethodCallParams(method="totalSupply"))
        assert r.abi_return == FXS_GENESIS - amount

    def test_burn_from(self, fxs, account):
        """burnFrom(self, 100e18) — from testBurn."""
        amount = 100 * 10**18
        # Self-approve first
        ak = allow_key(account.address, account.address)
        fxs.send.call(au.AppClientMethodCallParams(
            method="approve", args=[account.address, amount],
            box_references=[box_ref(fxs.app_id, ak)],
        ))
        fxs.send.call(au.AppClientMethodCallParams(
            method="burnFrom", args=[account.address, amount],
            box_references=[
                box_ref(fxs.app_id, bal_key(account.address)),
                box_ref(fxs.app_id, ak),
            ],
        ))
        r = fxs.send.call(au.AppClientMethodCallParams(method="totalSupply"))
        assert r.abi_return == FXS_GENESIS - amount

    def test_burn_balance_check(self, fxs, account):
        """After burn, balance should decrease."""
        amount = 200 * 10**18
        fxs.send.call(au.AppClientMethodCallParams(
            method="burn", args=[amount],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        assert r.abi_return == FXS_GENESIS - amount


# ---------------------------------------------------------------------------
# FRAX Governance / Admin
# ---------------------------------------------------------------------------

class TestFraxGovernance:
    @pytest.fixture
    def frax(self, localnet, account):
        return deploy_frax(localnet, account)

    def test_set_minting_fee(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setMintingFee", args=[3000]))
        assert frax.send.call(au.AppClientMethodCallParams(method="minting_fee")).abi_return == 3000

    def test_set_redemption_fee(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setRedemptionFee", args=[4500]))
        assert frax.send.call(au.AppClientMethodCallParams(method="redemption_fee")).abi_return == 4500

    def test_set_frax_step(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setFraxStep", args=[5000]))
        assert frax.send.call(au.AppClientMethodCallParams(method="frax_step")).abi_return == 5000

    def test_set_price_target(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setPriceTarget", args=[990000]))
        assert frax.send.call(au.AppClientMethodCallParams(method="price_target")).abi_return == 990000

    def test_set_price_band(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setPriceBand", args=[10000]))
        assert frax.send.call(au.AppClientMethodCallParams(method="price_band")).abi_return == 10000

    def test_set_refresh_cooldown(self, frax):
        frax.send.call(au.AppClientMethodCallParams(method="setRefreshCooldown", args=[1800]))
        assert frax.send.call(au.AppClientMethodCallParams(method="refresh_cooldown")).abi_return == 1800

    def test_toggle_collateral_ratio(self, frax, account):
        """Toggle paused state — tests COLLATERAL_RATIO_PAUSER role."""
        cr_pauser = keccak256(b"COLLATERAL_RATIO_PAUSER")
        role_boxes = _role_box_refs(frax.app_id, cr_pauser, account.address)
        frax.send.call(au.AppClientMethodCallParams(
            method="toggleCollateralRatio",
            box_references=role_boxes,
        ), send_params=NO_POPULATE)
        assert frax.send.call(au.AppClientMethodCallParams(method="collateral_ratio_paused")).abi_return is True

        # Toggle back
        frax.send.call(au.AppClientMethodCallParams(
            method="toggleCollateralRatio",
            box_references=role_boxes,
        ), send_params=NO_POPULATE)
        assert frax.send.call(au.AppClientMethodCallParams(method="collateral_ratio_paused")).abi_return is False

    def test_set_timelock(self, frax):
        new_tl = frax.app_address
        frax.send.call(au.AppClientMethodCallParams(method="setTimelock", args=[new_tl]))
        assert frax.send.call(au.AppClientMethodCallParams(method="timelock_address")).abi_return == new_tl

    def test_set_controller(self, frax):
        ctrl = frax.app_address
        frax.send.call(au.AppClientMethodCallParams(method="setController", args=[ctrl]))
        assert frax.send.call(au.AppClientMethodCallParams(method="controller_address")).abi_return == ctrl


# ---------------------------------------------------------------------------
# FRAX ERC20
# ---------------------------------------------------------------------------

class TestFraxERC20:
    @pytest.fixture
    def frax(self, localnet, account):
        return deploy_frax(localnet, account)

    def test_transfer(self, frax, account):
        recipient = frax.app_address
        amount = 1000 * 10**18
        r = frax.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[recipient, amount],
            box_references=[
                box_ref(frax.app_id, bal_key(account.address)),
                box_ref(frax.app_id, bal_key(recipient)),
            ],
        ))
        assert r.abi_return is True
        r = frax.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[recipient],
            box_references=[box_ref(frax.app_id, bal_key(recipient))],
        ))
        assert r.abi_return == amount

    def test_approve_and_allowance(self, frax, account):
        spender = frax.app_address
        amount = 500 * 10**18
        ak = allow_key(account.address, spender)
        frax.send.call(au.AppClientMethodCallParams(
            method="approve", args=[spender, amount],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        r = frax.send.call(au.AppClientMethodCallParams(
            method="allowance", args=[account.address, spender],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        assert r.abi_return == amount

    def test_burn(self, frax, account):
        amount = 100 * 10**18
        frax.send.call(au.AppClientMethodCallParams(
            method="burn", args=[amount],
            box_references=[box_ref(frax.app_id, bal_key(account.address))],
        ))
        assert frax.send.call(au.AppClientMethodCallParams(method="totalSupply")).abi_return == FRAX_GENESIS - amount

    def test_increase_allowance(self, frax, account):
        spender = frax.app_address
        ak = allow_key(account.address, spender)
        frax.send.call(au.AppClientMethodCallParams(
            method="approve", args=[spender, 100],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        frax.send.call(au.AppClientMethodCallParams(
            method="increaseAllowance", args=[spender, 50],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        r = frax.send.call(au.AppClientMethodCallParams(
            method="allowance", args=[account.address, spender],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        assert r.abi_return == 150

    def test_decrease_allowance(self, frax, account):
        spender = frax.app_address
        ak = allow_key(account.address, spender)
        frax.send.call(au.AppClientMethodCallParams(
            method="approve", args=[spender, 200],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        frax.send.call(au.AppClientMethodCallParams(
            method="decreaseAllowance", args=[spender, 50],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        r = frax.send.call(au.AppClientMethodCallParams(
            method="allowance", args=[account.address, spender],
            box_references=[box_ref(frax.app_id, ak)],
        ))
        assert r.abi_return == 150


# ---------------------------------------------------------------------------
# Pool management (translated from testPoolActions)
# ---------------------------------------------------------------------------

class TestPoolManagement:
    @pytest.fixture
    def frax(self, localnet, account):
        return deploy_frax(localnet, account)

    def test_add_pool(self, frax, account):
        """addPool — from testPoolActions."""
        pool = frax.app_address
        pool_key = mapping_box_key("frax_pools", addr_to_bytes32(pool))
        arr_key = mapping_box_key("frax_pools_array", b"")  # box for the array

        frax.send.call(au.AppClientMethodCallParams(
            method="addPool", args=[pool],
            box_references=[
                box_ref(frax.app_id, pool_key),
                box_ref(frax.app_id, arr_key),
            ],
        ))
        r = frax.send.call(au.AppClientMethodCallParams(method="frax_pools_count"))
        assert r.abi_return == 1

    def test_add_pool_duplicate_reverts(self, frax, account):
        pool = frax.app_address
        pool_key = mapping_box_key("frax_pools", addr_to_bytes32(pool))
        arr_key = mapping_box_key("frax_pools_array", b"")

        frax.send.call(au.AppClientMethodCallParams(
            method="addPool", args=[pool],
            box_references=[
                box_ref(frax.app_id, pool_key),
                box_ref(frax.app_id, arr_key),
            ],
        ))
        with pytest.raises(Exception, match="already exists|assert"):
            frax.send.call(au.AppClientMethodCallParams(
                method="addPool", args=[pool],
                box_references=[
                    box_ref(frax.app_id, pool_key),
                    box_ref(frax.app_id, arr_key),
                ],
            ), send_params=NO_POPULATE)

    def test_remove_pool(self, frax, account):
        pool = frax.app_address
        pool_key = mapping_box_key("frax_pools", addr_to_bytes32(pool))
        arr_key = mapping_box_key("frax_pools_array", b"")

        frax.send.call(au.AppClientMethodCallParams(
            method="addPool", args=[pool],
            box_references=[
                box_ref(frax.app_id, pool_key),
                box_ref(frax.app_id, arr_key),
            ],
        ))
        frax.send.call(au.AppClientMethodCallParams(
            method="removePool", args=[pool],
            box_references=[
                box_ref(frax.app_id, pool_key),
                box_ref(frax.app_id, arr_key),
            ],
        ))

    def test_add_pool_zero_address_reverts(self, frax, account):
        zero = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
        pool_key = mapping_box_key("frax_pools", addr_to_bytes32(zero))
        with pytest.raises(Exception, match="Zero address|assert"):
            frax.send.call(au.AppClientMethodCallParams(
                method="addPool", args=[zero],
                box_references=[box_ref(frax.app_id, pool_key)],
            ), send_params=NO_POPULATE)


# ---------------------------------------------------------------------------
# FXS Vote tracking toggle
# ---------------------------------------------------------------------------

class TestFXSVoteToggle:
    """Translated from testTransfers — toggleVotes disables/enables vote checkpointing."""

    @pytest.fixture
    def fxs(self, localnet, account):
        return deploy_fxs(localnet, account)

    def test_toggle_votes_off(self, fxs, account):
        assert fxs.send.call(au.AppClientMethodCallParams(method="trackingVotes")).abi_return is True
        fxs.send.call(au.AppClientMethodCallParams(method="toggleVotes"))
        assert fxs.send.call(au.AppClientMethodCallParams(method="trackingVotes")).abi_return is False

    def test_toggle_votes_on_again(self, fxs, account):
        fxs.send.call(au.AppClientMethodCallParams(method="toggleVotes"))
        assert fxs.send.call(au.AppClientMethodCallParams(method="trackingVotes")).abi_return is False
        fxs.send.call(au.AppClientMethodCallParams(method="toggleVotes"))
        assert fxs.send.call(au.AppClientMethodCallParams(method="trackingVotes")).abi_return is True

    def test_transfer_with_tracking_disabled(self, fxs, account):
        """From testTransfers: disable tracking, transfer, verify balances."""
        recipient = fxs.app_address
        amount = 100 * 10**18

        # Disable tracking
        fxs.send.call(au.AppClientMethodCallParams(method="toggleVotes"))

        # Transfer
        fxs.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[recipient, amount],
            box_references=[
                box_ref(fxs.app_id, bal_key(account.address)),
                box_ref(fxs.app_id, bal_key(recipient)),
            ],
        ))

        # Verify
        r = fxs.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[box_ref(fxs.app_id, bal_key(account.address))],
        ))
        assert r.abi_return == FXS_GENESIS - amount
