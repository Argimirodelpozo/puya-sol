"""
Auction2 behavioral tests.
Tests lot creation, bidding, and closing.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def reserve_key(lid):
    return mapping_box_key("_lotReserve", lid.to_bytes(64, "big"))


def high_bid_key(lid):
    return mapping_box_key("_lotHighBid", lid.to_bytes(64, "big"))


def bidder_key(lid):
    return mapping_box_key("_lotHighBidder", lid.to_bytes(64, "big"))


def closed_key(lid):
    return mapping_box_key("_lotClosed", lid.to_bytes(64, "big"))


def bid_count_key(lid):
    return mapping_box_key("_lotBidCount", lid.to_bytes(64, "big"))


def lot_boxes(lid):
    return [
        au.BoxReference(app_id=0, name=reserve_key(lid)),
        au.BoxReference(app_id=0, name=high_bid_key(lid)),
        au.BoxReference(app_id=0, name=bidder_key(lid)),
        au.BoxReference(app_id=0, name=closed_key(lid)),
        au.BoxReference(app_id=0, name=bid_count_key(lid)),
    ]


@pytest.fixture(scope="module")
def auc(localnet, account):
    return deploy_contract(localnet, account, "Auction2Test")


def test_deploy(auc):
    assert auc.app_id > 0


def test_admin(auc, account):
    result = auc.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_create_lot(auc):
    boxes = lot_boxes(0)
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="createLot",
            args=[1000],  # reserve=1000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_lot_count(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(method="lotCount")
    )
    assert result.abi_return == 1


def test_lot_reserve(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="getLotReserve",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=reserve_key(0))],
        )
    )
    assert result.abi_return == 1000


def test_place_bid(auc, account):
    auc.send.call(
        au.AppClientMethodCallParams(
            method="placeBid",
            args=[0, account.address, 1500],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=reserve_key(0)),
                au.BoxReference(app_id=0, name=high_bid_key(0)),
                au.BoxReference(app_id=0, name=bidder_key(0)),
                au.BoxReference(app_id=0, name=bid_count_key(0)),
            ],
        )
    )


def test_high_bid(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="getLotHighBid",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=high_bid_key(0))],
        )
    )
    assert result.abi_return == 1500


def test_high_bidder(auc, account):
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="getLotHighBidder",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=bidder_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_bid_count(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="getLotBidCount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=bid_count_key(0))],
        )
    )
    assert result.abi_return == 1


def test_place_higher_bid(auc, account):
    auc.send.call(
        au.AppClientMethodCallParams(
            method="placeBid",
            args=[0, account.address, 2000],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=reserve_key(0)),
                au.BoxReference(app_id=0, name=high_bid_key(0)),
                au.BoxReference(app_id=0, name=bidder_key(0)),
                au.BoxReference(app_id=0, name=bid_count_key(0)),
            ],
            note=b"b2",
        )
    )
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="getLotHighBid",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=high_bid_key(0))],
            note=b"hb2",
        )
    )
    assert result.abi_return == 2000


def test_total_bids(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(method="totalBids")
    )
    assert result.abi_return == 2


def test_close_lot(auc):
    auc.send.call(
        au.AppClientMethodCallParams(
            method="closeLot",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=high_bid_key(0)),
            ],
        )
    )


def test_lot_closed(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="isLotClosed",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=closed_key(0))],
        )
    )
    assert result.abi_return is True


def test_total_revenue(auc):
    result = auc.send.call(
        au.AppClientMethodCallParams(method="totalRevenue")
    )
    assert result.abi_return == 2000


def test_create_second_lot(auc):
    boxes = lot_boxes(1)
    result = auc.send.call(
        au.AppClientMethodCallParams(
            method="createLot",
            args=[500],
            box_references=boxes,
            note=b"l2",
        )
    )
    assert result.abi_return == 1
