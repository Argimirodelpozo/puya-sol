"""
Escrow behavioral tests.
Tests deposit, withdrawal, and access control.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def deposit_key(addr):
    return mapping_box_key("_deposits", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def escrow(localnet, account):
    return deploy_contract(localnet, account, "EscrowTest")


def test_deploy(escrow):
    assert escrow.app_id > 0


def test_owner(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_total_deposits(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="totalDeposits")
    )
    assert result.abi_return == 0


def test_deposit(escrow, account):
    key = deposit_key(account.address)
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[account.address, 5000],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_deposits_of_after_deposit(escrow, account):
    key = deposit_key(account.address)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="depositsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 5000


def test_total_deposits_after_deposit(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="totalDeposits")
    )
    assert result.abi_return == 5000


def test_withdraw(escrow, account):
    key = deposit_key(account.address)
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[account.address, 2000],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_deposits_after_withdraw(escrow, account):
    key = deposit_key(account.address)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="depositsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"after_withdraw",
        )
    )
    assert result.abi_return == 3000


def test_total_after_withdraw(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="totalDeposits", note=b"total2")
    )
    assert result.abi_return == 3000
