"""
Lending behavioral tests.
Tests deposit, borrow, repay, interest, and liquidity.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def dep_key(addr):
    return mapping_box_key("_deposited", encoding.decode_address(addr))


def bor_key(addr):
    return mapping_box_key("_borrowed", encoding.decode_address(addr))


def idx_key(addr):
    return mapping_box_key("_borrowerIndex", encoding.decode_address(addr))


def account_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=dep_key(addr)),
        au.BoxReference(app_id=0, name=bor_key(addr)),
        au.BoxReference(app_id=0, name=idx_key(addr)),
    ]


@pytest.fixture(scope="module")
def lending(localnet, account):
    return deploy_contract(localnet, account, "LendingTest")


def test_deploy(lending):
    assert lending.app_id > 0


def test_admin(lending, account):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_interest_rate(lending):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getInterestRate")
    )
    assert result.abi_return == 500


def test_init_and_lend(lending, account):
    boxes = account_boxes(account.address)
    lending.send.call(
        au.AppClientMethodCallParams(
            method="initAccount",
            args=[account.address],
            box_references=boxes,
        )
    )
    lending.send.call(
        au.AppClientMethodCallParams(
            method="lend",
            args=[account.address, 50000],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )


def test_deposited(lending, account):
    result = lending.send.call(
        au.AppClientMethodCallParams(
            method="getDeposited",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    assert result.abi_return == 50000


def test_total_deposited(lending):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getTotalDeposited")
    )
    assert result.abi_return == 50000


def test_available_liquidity(lending):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getAvailableLiquidity")
    )
    assert result.abi_return == 50000


def test_borrow(lending, account):
    lending.send.call(
        au.AppClientMethodCallParams(
            method="borrow",
            args=[account.address, 20000],
            box_references=account_boxes(account.address),
        )
    )


def test_borrowed(lending, account):
    result = lending.send.call(
        au.AppClientMethodCallParams(
            method="getBorrowed",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bor_key(account.address))],
        )
    )
    assert result.abi_return == 20000


def test_liquidity_after_borrow(lending):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getAvailableLiquidity")
    )
    assert result.abi_return == 30000


def test_borrower_count(lending):
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getBorrowerCount")
    )
    assert result.abi_return == 1


def test_interest(lending, account):
    # 20000 * 500 / 10000 = 1000
    result = lending.send.call(
        au.AppClientMethodCallParams(
            method="calculateInterest",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bor_key(account.address))],
        )
    )
    assert result.abi_return == 1000


def test_repay(lending, account):
    lending.send.call(
        au.AppClientMethodCallParams(
            method="repay",
            args=[account.address, 5000],
            box_references=[au.BoxReference(app_id=0, name=bor_key(account.address))],
        )
    )
    result = lending.send.call(
        au.AppClientMethodCallParams(
            method="getBorrowed",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bor_key(account.address))],
            note=b"bor2",
        )
    )
    assert result.abi_return == 15000


def test_set_interest_rate(lending):
    lending.send.call(
        au.AppClientMethodCallParams(
            method="setInterestRate",
            args=[1000],  # 10%
        )
    )
    result = lending.send.call(
        au.AppClientMethodCallParams(method="getInterestRate", note=b"ir2")
    )
    assert result.abi_return == 1000
