"""
SimpleEscrow behavioral tests.
Tests deposits, withdrawals, availability, and emergency freeze.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def dep_key(addr):
    return mapping_box_key("_deposits", encoding.decode_address(addr))


def withdrawn_key(addr):
    return mapping_box_key("_withdrawn", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_depositorIndex", encoding.decode_address(addr))


def depositor_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=dep_key(addr)),
        au.BoxReference(app_id=0, name=withdrawn_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
    ]


@pytest.fixture(scope="module")
def escrow(localnet, account):
    return deploy_contract(localnet, account, "SimpleEscrowTest")


def test_deploy(escrow):
    assert escrow.app_id > 0


def test_admin(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_not_frozen(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="isFrozen")
    )
    assert result.abi_return is False


def test_init_and_deposit(escrow, account):
    boxes = depositor_boxes(account.address)
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="initDepositor",
            args=[account.address],
            box_references=boxes,
        )
    )
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )


def test_deposits(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getDeposits",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_is_depositor(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="isDepositor",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=index_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_depositor_count(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="getDepositorCount")
    )
    assert result.abi_return == 1


def test_total_deposits(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="getTotalDeposits")
    )
    assert result.abi_return == 5000


def test_available(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="available",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=dep_key(account.address)),
                au.BoxReference(app_id=0, name=withdrawn_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 5000


def test_withdraw(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[account.address, 2000],
            box_references=[
                au.BoxReference(app_id=0, name=dep_key(account.address)),
                au.BoxReference(app_id=0, name=withdrawn_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 2000


def test_available_after_withdraw(escrow, account):
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="available",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=dep_key(account.address)),
                au.BoxReference(app_id=0, name=withdrawn_key(account.address)),
            ],
            note=b"avail2",
        )
    )
    assert result.abi_return == 3000


def test_total_withdrawn(escrow):
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="getTotalWithdrawn")
    )
    assert result.abi_return == 2000


def test_deposit_more(escrow, account):
    boxes = depositor_boxes(account.address)
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[account.address, 3000],
            box_references=boxes,
            note=b"dep2",
        )
    )
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="available",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=dep_key(account.address)),
                au.BoxReference(app_id=0, name=withdrawn_key(account.address)),
            ],
            note=b"avail3",
        )
    )
    assert result.abi_return == 6000  # 8000 deposited - 2000 withdrawn


def test_emergency_freeze(escrow):
    escrow.send.call(
        au.AppClientMethodCallParams(method="emergencyFreeze")
    )
    result = escrow.send.call(
        au.AppClientMethodCallParams(method="isFrozen", note=b"frz2")
    )
    assert result.abi_return is True
