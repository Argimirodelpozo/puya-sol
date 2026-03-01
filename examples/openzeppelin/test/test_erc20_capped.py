"""
OpenZeppelin ERC20Capped behavioral tests.
Tests cap enforcement on minting.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


@pytest.fixture(scope="module")
def erc20_capped(localnet, account):
    return deploy_contract(localnet, account, "ERC20CappedTest")


def test_deploy(erc20_capped):
    assert erc20_capped.app_id > 0


def test_cap(erc20_capped):
    result = erc20_capped.send.call(
        au.AppClientMethodCallParams(method="cap")
    )
    assert result.abi_return == 1000000


def test_name(erc20_capped):
    result = erc20_capped.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "CappedToken"


def test_symbol(erc20_capped):
    result = erc20_capped.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "CAP"


def test_total_supply_initially_zero(erc20_capped):
    result = erc20_capped.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == 0


def test_mint_within_cap(erc20_capped, account):
    """Mint 500000 tokens — should succeed within cap."""
    balance_key = mapping_box_key("_balances", addr_bytes(account.address))
    erc20_capped.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 500000],
            box_references=[box_ref(erc20_capped.app_id, balance_key)],
        )
    )
    result = erc20_capped.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(erc20_capped.app_id, balance_key)],
        )
    )
    assert result.abi_return == 500000


def test_mint_exceeds_cap_fails(erc20_capped, account):
    """Minting beyond cap should fail."""
    balance_key = mapping_box_key("_balances", addr_bytes(account.address))
    with pytest.raises(Exception):
        erc20_capped.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[account.address, 500001],
                box_references=[box_ref(erc20_capped.app_id, balance_key)],
            )
        )
