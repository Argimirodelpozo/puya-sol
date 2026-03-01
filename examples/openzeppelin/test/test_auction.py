"""
Auction behavioral tests.
Tests bidding, withdrawing, and ending the auction.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def pending_key(addr):
    return mapping_box_key("_pendingReturns", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def auction(localnet, account):
    return deploy_contract(localnet, account, "AuctionTest")


def test_deploy(auction):
    assert auction.app_id > 0


def test_owner(auction, account):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_highest_bid(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="highestBid")
    )
    assert result.abi_return == 0


def test_initial_not_ended(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="ended")
    )
    assert result.abi_return is False


def test_bid(auction, account):
    key = pending_key(account.address)
    auction.send.call(
        au.AppClientMethodCallParams(
            method="bid",
            args=[account.address, 100],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_highest_bid_after_bid(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="highestBid", note=b"hb1")
    )
    assert result.abi_return == 100


def test_highest_bidder(auction, account):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="highestBidder")
    )
    assert result.abi_return == account.address


def test_outbid(auction, account):
    """Second bid should put first bid into pending returns."""
    key = pending_key(account.address)
    auction.send.call(
        au.AppClientMethodCallParams(
            method="bid",
            args=[account.address, 200],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_pending_return_after_outbid(auction, account):
    key = pending_key(account.address)
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="pendingReturn",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 100  # first bid returned


def test_withdraw(auction, account):
    key = pending_key(account.address)
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return == 100


def test_pending_after_withdraw(auction, account):
    key = pending_key(account.address)
    result = auction.send.call(
        au.AppClientMethodCallParams(
            method="pendingReturn",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"after_wd",
        )
    )
    assert result.abi_return == 0


def test_end_auction(auction):
    auction.send.call(
        au.AppClientMethodCallParams(method="endAuction")
    )


def test_ended_after_end(auction):
    result = auction.send.call(
        au.AppClientMethodCallParams(method="ended", note=b"ended2")
    )
    assert result.abi_return is True
