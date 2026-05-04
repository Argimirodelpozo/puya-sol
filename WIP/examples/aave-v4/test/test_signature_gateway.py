"""SignatureGateway constant tests — ported from upstream
tests/contracts/position-manager/SignatureGateway/SignatureGateway.Constants.t.sol.

EIP-712 typehashes are pure-view; they're compile-time keccak256 of
the canonical type strings. Each test asserts the on-chain constant
matches the computed keccak256 of the corresponding type-string.

Skipped from upstream:
 - test_constructor (vm.expectRevert on `new SignatureGateway(0)` —
   AVM construction path differs; the matching check happens at
   __postInit which our deploy_contract calls automatically)
 - test_eip712Domain / test_DOMAIN_SEPARATOR (compute compares against
   block.chainid + verifyingContract; AVM exposes neither in the same
   shape — we'd assert structural form only, which is low-value)
 - All `*WithSig` flows (ECDSA signing path)
"""

from __future__ import annotations

import os

import algokit_utils as au
import pytest
from algosdk import encoding
from Crypto.Hash import keccak
from conftest import deploy_contract


def _addr_pk32(addr: str) -> bytes:
    return encoding.decode_address(addr)


def _keccak256(s: str) -> bytes:
    """Solidity-style: keccak256(utf8(s))."""
    k = keccak.new(digest_bits=256)
    k.update(s.encode())
    return k.digest()


@pytest.fixture(scope="module")
def sg(localnet, account):
    return deploy_contract(
        localnet, account, "SignatureGateway",
        app_args=[_addr_pk32(account.address)],
    )


def _call(client, method):
    result = client.send.call(au.AppClientMethodCallParams(
        method=method, args=[], note=os.urandom(8),
    ))
    return result.abi_return


def _to_bytes(value) -> bytes:
    """Algokit decodes ABI byte[32] as a list[int]; normalise to bytes."""
    return bytes(value) if isinstance(value, list) else value


def test_deploy(sg):
    assert sg.app_id > 0


def test_owner(sg, account):
    assert _call(sg, "owner") == account.address


def test_rescueGuardian(sg, account):
    """Constructor sets rescueGuardian == initialOwner."""
    assert _call(sg, "rescueGuardian") == account.address


# ─── EIP-712 typehashes — keccak256 of canonical type strings ────────────────
# byte[32] returns from algokit surface as bytes. We compare directly
# against the computed keccak256 to avoid hex-bytes encoding ambiguity.


def test_supply_typeHash(sg):
    expected = _keccak256(
        "Supply(address spoke,uint256 reserveId,uint256 amount,"
        "address onBehalfOf,uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "SUPPLY_TYPEHASH")) == expected


def test_withdraw_typeHash(sg):
    expected = _keccak256(
        "Withdraw(address spoke,uint256 reserveId,uint256 amount,"
        "address onBehalfOf,uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "WITHDRAW_TYPEHASH")) == expected


def test_borrow_typeHash(sg):
    expected = _keccak256(
        "Borrow(address spoke,uint256 reserveId,uint256 amount,"
        "address onBehalfOf,uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "BORROW_TYPEHASH")) == expected


def test_repay_typeHash(sg):
    expected = _keccak256(
        "Repay(address spoke,uint256 reserveId,uint256 amount,"
        "address onBehalfOf,uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "REPAY_TYPEHASH")) == expected


def test_setUsingAsCollateral_typeHash(sg):
    expected = _keccak256(
        "SetUsingAsCollateral(address spoke,uint256 reserveId,"
        "bool useAsCollateral,address onBehalfOf,uint256 nonce,"
        "uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "SET_USING_AS_COLLATERAL_TYPEHASH")) == expected


def test_updateUserRiskPremium_typeHash(sg):
    expected = _keccak256(
        "UpdateUserRiskPremium(address spoke,address onBehalfOf,"
        "uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "UPDATE_USER_RISK_PREMIUM_TYPEHASH")) == expected


def test_updateUserDynamicConfig_typeHash(sg):
    expected = _keccak256(
        "UpdateUserDynamicConfig(address spoke,address onBehalfOf,"
        "uint256 nonce,uint256 deadline)"
    )
    assert _to_bytes(_call(sg, "UPDATE_USER_DYNAMIC_CONFIG_TYPEHASH")) == expected
