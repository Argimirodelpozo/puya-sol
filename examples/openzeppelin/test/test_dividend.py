"""
Dividend behavioral tests.
Tests shareholder management, dividend distribution, and claiming.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def shares_key(addr):
    return mapping_box_key("_shares", encoding.decode_address(addr))


def claimed_key(addr):
    return mapping_box_key("_claimed", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_shareholderIndex", encoding.decode_address(addr))


def holder_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=shares_key(addr)),
        au.BoxReference(app_id=0, name=claimed_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
    ]


@pytest.fixture(scope="module")
def div(localnet, account):
    return deploy_contract(localnet, account, "DividendTest")


def test_deploy(div):
    assert div.app_id > 0


def test_admin(div, account):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_add_shareholder(div, account):
    boxes = holder_boxes(account.address)
    div.send.call(
        au.AppClientMethodCallParams(
            method="initHolder",
            args=[account.address],
            box_references=boxes,
        )
    )
    div.send.call(
        au.AppClientMethodCallParams(
            method="addShareHolder",
            args=[account.address, 100],
            box_references=boxes,
        )
    )


def test_shares(div, account):
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="getShares",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=shares_key(account.address))],
        )
    )
    assert result.abi_return == 100


def test_shareholder_count(div):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getShareholderCount")
    )
    assert result.abi_return == 1


def test_total_shares(div):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getTotalShares")
    )
    assert result.abi_return == 100


def test_distribute(div):
    div.send.call(
        au.AppClientMethodCallParams(
            method="distributeDividend",
            args=[10000],
        )
    )


def test_total_dividends(div):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getTotalDividends")
    )
    assert result.abi_return == 10000


def test_dividend_round(div):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getDividendRound")
    )
    assert result.abi_return == 1


def test_claimable(div, account):
    # 10000 * 100 / 100 - 0 = 10000
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="claimable",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=claimed_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 10000


def test_claim(div, account):
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="claim",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=claimed_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 10000


def test_claimed(div, account):
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="getClaimed",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=claimed_key(account.address))],
        )
    )
    assert result.abi_return == 10000


def test_total_claimed(div):
    result = div.send.call(
        au.AppClientMethodCallParams(method="getTotalClaimed")
    )
    assert result.abi_return == 10000


def test_claimable_after(div, account):
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="claimable",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=claimed_key(account.address)),
            ],
            note=b"cl2",
        )
    )
    assert result.abi_return == 0


def test_distribute_more_and_claim(div, account):
    div.send.call(
        au.AppClientMethodCallParams(
            method="distributeDividend",
            args=[5000],
            note=b"dist2",
        )
    )
    # Now total=15000, claimed=10000, claimable = 15000 - 10000 = 5000
    result = div.send.call(
        au.AppClientMethodCallParams(
            method="claim",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=shares_key(account.address)),
                au.BoxReference(app_id=0, name=claimed_key(account.address)),
            ],
            note=b"claim2",
        )
    )
    assert result.abi_return == 5000
