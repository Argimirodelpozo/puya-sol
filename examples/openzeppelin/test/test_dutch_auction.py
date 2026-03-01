"""
DutchAuction behavioral tests.
Tests descending price auction: start, price calculation, buy, and getters.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def auction(localnet, account):
    return deploy_contract(localnet, account, "DutchAuctionTest")


def test_deploy(auction):
    assert auction.app_id > 0


def test_owner(auction, account):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getOwner")
    )
    assert result.abi_return == account.address


def test_item(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getItem")
    )
    assert result.abi_return == 42


def test_not_sold_initially(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="isSold")
    )
    assert result.abi_return is False


def test_total_auctions_initial(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getTotalAuctions")
    )
    assert result.abi_return == 0


def test_start_auction(auction):
    auction.send.call(
        au.AppClientMethodCallParams(
            method="startAuction",
            args=[1000],  # currentTime = 1000
        )
    )


def test_price_at_start(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="getCurrentPrice",
            args=[1000],  # currentTime = startTime
        )
    )
    assert result.abi_return == 10000  # startPrice


def test_price_at_midpoint(auction):
    # At time 1050 (50% elapsed), price = 10000 - (10000-1000)*50/100 = 10000 - 4500 = 5500
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="getCurrentPrice",
            args=[1050],
        )
    )
    assert result.abi_return == 5500


def test_price_at_end(auction):
    # At time 1100+ (duration expired), price = endPrice = 1000
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="getCurrentPrice",
            args=[1200],
        )
    )
    assert result.abi_return == 1000


def test_buy(auction):
    # Buy at time 1025 (25% elapsed): price = 10000 - 9000*25/100 = 10000 - 2250 = 7750
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="buy",
            args=[1025],
        )
    )
    assert result.abi_return == 7750


def test_sold_after_buy(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="isSold")
    )
    assert result.abi_return is True


def test_buyer(auction, account):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getBuyer")
    )
    assert result.abi_return == account.address


def test_final_price(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getFinalPrice")
    )
    assert result.abi_return == 7750


def test_total_auctions_after(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="getTotalAuctions")
    )
    assert result.abi_return == 1


def test_restart_auction(auction):
    # Start a new auction round
    auction.send.call(
        au.AppClientMethodCallParams(
            method="startAuction",
            args=[2000],
            note=b"start2",
        )
    )
    result = auction.send.call(
        au.AppClientMethodCallParams(method="isSold", note=b"sold2")
    )
    assert result.abi_return is False
