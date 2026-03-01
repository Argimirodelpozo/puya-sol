"""
ERC4626 Vault behavioral tests.
Tests deposit, withdraw, redeem, and share accounting.
"""
import hashlib
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def shares_key(addr):
    return mapping_box_key("_shares", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def vault(localnet, account):
    return deploy_contract(localnet, account, "ERC4626Test")


def test_deploy(vault):
    assert vault.app_id > 0


def test_name(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "Vault Token"


def test_symbol(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "vTKN"


def test_convert_to_shares(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="convertToShares", args=[1000],
        )
    )
    assert result.abi_return == 1000


def test_convert_to_assets(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="convertToAssets", args=[1000],
        )
    )
    assert result.abi_return == 1000


def test_max_deposit(vault, account):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="maxDeposit", args=[account.address],
        )
    )
    assert result.abi_return == 2**256 - 1


def test_deposit(vault, account):
    key = shares_key(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[5000, account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 5000  # shares received


def test_balance_after_deposit(vault, account):
    key = shares_key(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 5000


def test_total_supply_after_deposit(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == 5000


def test_total_assets_after_deposit(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="totalAssets")
    )
    assert result.abi_return == 5000


def test_withdraw(vault, account):
    key = shares_key(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[2000, account.address, account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 2000  # shares burned


def test_balance_after_withdraw(vault, account):
    key = shares_key(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"after_withdraw",
        )
    )
    assert result.abi_return == 3000


def test_redeem(vault, account):
    key = shares_key(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="redeem",
            args=[1000, account.address, account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 1000  # assets received


def test_final_state(vault, account):
    key = shares_key(account.address)
    bal = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"final",
        )
    )
    assert bal.abi_return == 2000

    total = vault.send.call(
        au.AppClientMethodCallParams(method="totalSupply", note=b"final_ts")
    )
    assert total.abi_return == 2000
