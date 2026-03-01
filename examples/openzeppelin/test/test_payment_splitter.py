"""
PaymentSplitter behavioral tests.
Tests share-based payment distribution, deposit, release, and releasable calculations.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def shares_key(addr):
    return mapping_box_key("_shares", encoding.decode_address(addr))


def released_key(addr):
    return mapping_box_key("_released", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_payeeIndex", encoding.decode_address(addr))


def payee_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=shares_key(addr)),
        au.BoxReference(app_id=0, name=released_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
    ]


@pytest.fixture(scope="module")
def splitter(localnet, account):
    return deploy_contract(localnet, account, "PaymentSplitterTest")


def test_deploy(splitter):
    assert splitter.app_id > 0


def test_owner(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(method="getOwner")
    )
    assert result.abi_return == account.address


def test_add_payee_a(splitter, account):
    boxes = payee_boxes(account.address)
    splitter.send.call(
        au.AppClientMethodCallParams(
            method="initPayee",
            args=[account.address],
            box_references=boxes,
        )
    )
    splitter.send.call(
        au.AppClientMethodCallParams(
            method="addPayee",
            args=[account.address, 60],
            box_references=boxes,
        )
    )


def test_is_payee(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="isPayee",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=index_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_shares(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="getShares",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=shares_key(account.address))],
        )
    )
    assert result.abi_return == 60


def test_payee_count(splitter):
    result = splitter.send.call(
        au.AppClientMethodCallParams(method="getPayeeCount")
    )
    assert result.abi_return == 1


def test_total_shares(splitter):
    result = splitter.send.call(
        au.AppClientMethodCallParams(method="getTotalShares")
    )
    assert result.abi_return == 60


def test_deposit(splitter):
    splitter.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[12000],
        )
    )


def test_total_deposited(splitter):
    result = splitter.send.call(
        au.AppClientMethodCallParams(method="getTotalDeposited")
    )
    assert result.abi_return == 12000


def test_releasable(splitter, account):
    # 12000 * 60 / 60 = 12000 (only one payee with 60 shares)
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="releasable",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 12000


def test_release(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 12000


def test_released(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="getReleased",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=released_key(account.address))],
        )
    )
    assert result.abi_return == 12000


def test_total_released(splitter):
    result = splitter.send.call(
        au.AppClientMethodCallParams(method="getTotalReleased")
    )
    assert result.abi_return == 12000


def test_releasable_after(splitter, account):
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="releasable",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
            note=b"rel2",
        )
    )
    assert result.abi_return == 0


def test_deposit_more_and_release(splitter, account):
    splitter.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[6000],
            note=b"dep2",
        )
    )
    # Total deposited = 18000, released = 12000, releasable = 18000 - 12000 = 6000
    result = splitter.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
            note=b"rel3",
        )
    )
    assert result.abi_return == 6000
