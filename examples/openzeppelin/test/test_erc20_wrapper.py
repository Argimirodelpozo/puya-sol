"""
ERC20Wrapper behavioral tests.
Tests deposit, withdraw, and transfer of wrapped tokens.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def bal_key(addr):
    return mapping_box_key("_balances", encoding.decode_address(addr))


def dep_key(addr):
    return mapping_box_key("_deposited", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def wrapper(localnet, account):
    return deploy_contract(localnet, account, "ERC20WrapperTest")


def test_deploy(wrapper):
    assert wrapper.app_id > 0


def test_name(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "Wrapped Token"


def test_symbol(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "wTOK"


def test_initial_supply(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == 0


def test_deposit(wrapper, account):
    boxes = [
        au.BoxReference(app_id=0, name=dep_key(account.address)),
        au.BoxReference(app_id=0, name=bal_key(account.address)),
    ]
    result = wrapper.send.call(
        au.AppClientMethodCallParams(
            method="depositFor",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_balance_after_deposit(wrapper, account):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_deposited_of(wrapper, account):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(
            method="depositedOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_total_supply_after_deposit(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="totalSupply", note=b"ts1")
    )
    assert result.abi_return == 5000


def test_total_deposited(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="totalDeposited")
    )
    assert result.abi_return == 5000


def test_withdraw(wrapper, account):
    boxes = [
        au.BoxReference(app_id=0, name=dep_key(account.address)),
        au.BoxReference(app_id=0, name=bal_key(account.address)),
    ]
    result = wrapper.send.call(
        au.AppClientMethodCallParams(
            method="withdrawTo",
            args=[account.address, 2000],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_balance_after_withdraw(wrapper, account):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
            note=b"bal2",
        )
    )
    assert result.abi_return == 3000


def test_supply_after_withdraw(wrapper):
    result = wrapper.send.call(
        au.AppClientMethodCallParams(method="totalSupply", note=b"ts2")
    )
    assert result.abi_return == 3000
