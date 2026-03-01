"""
Oracle behavioral tests.
Tests feed creation, price updates, staleness checks, and activation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def price_key(fid):
    return mapping_box_key("_feedPrice", fid.to_bytes(64, "big"))


def ts_key(fid):
    return mapping_box_key("_feedTimestamp", fid.to_bytes(64, "big"))


def active_key(fid):
    return mapping_box_key("_feedActive", fid.to_bytes(64, "big"))


def dec_key(fid):
    return mapping_box_key("_feedDecimals", fid.to_bytes(64, "big"))


def feed_boxes(fid):
    return [
        au.BoxReference(app_id=0, name=price_key(fid)),
        au.BoxReference(app_id=0, name=ts_key(fid)),
        au.BoxReference(app_id=0, name=active_key(fid)),
        au.BoxReference(app_id=0, name=dec_key(fid)),
    ]


@pytest.fixture(scope="module")
def oracle(localnet, account):
    return deploy_contract(localnet, account, "OracleTest")


def test_deploy(oracle):
    assert oracle.app_id > 0


def test_admin(oracle, account):
    result = oracle.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_create_feed(oracle):
    boxes = feed_boxes(0)
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="createFeed",
            args=[8],  # 8 decimals
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_feed_count(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(method="getFeedCount")
    )
    assert result.abi_return == 1


def test_is_active(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="isFeedActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    assert result.abi_return is True


def test_decimals(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="getDecimals",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=dec_key(0))],
        )
    )
    assert result.abi_return == 8


def test_update_price(oracle):
    oracle.send.call(
        au.AppClientMethodCallParams(
            method="updatePrice",
            args=[0, 5000000, 1000],  # price=50.00000000 at time=1000
            box_references=[
                au.BoxReference(app_id=0, name=price_key(0)),
                au.BoxReference(app_id=0, name=ts_key(0)),
                au.BoxReference(app_id=0, name=active_key(0)),
            ],
        )
    )


def test_price(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="getPrice",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=price_key(0))],
        )
    )
    assert result.abi_return == 5000000


def test_timestamp(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="getTimestamp",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=ts_key(0))],
        )
    )
    assert result.abi_return == 1000


def test_update_count(oracle):
    result = oracle.send.call(
        au.AppClientMethodCallParams(method="getUpdateCount")
    )
    assert result.abi_return == 1


def test_not_stale(oracle):
    # currentTime=1050, maxAge=100 → 1050-1000=50 < 100 → not stale
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="isPriceStale",
            args=[0, 1050, 100],
            box_references=[au.BoxReference(app_id=0, name=ts_key(0))],
        )
    )
    assert result.abi_return is False


def test_stale(oracle):
    # currentTime=1200, maxAge=100 → 1200-1000=200 > 100 → stale
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="isPriceStale",
            args=[0, 1200, 100],
            box_references=[au.BoxReference(app_id=0, name=ts_key(0))],
            note=b"stale2",
        )
    )
    assert result.abi_return is True


def test_deactivate_feed(oracle):
    oracle.send.call(
        au.AppClientMethodCallParams(
            method="deactivateFeed",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="isFeedActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"act2",
        )
    )
    assert result.abi_return is False


def test_activate_feed(oracle):
    oracle.send.call(
        au.AppClientMethodCallParams(
            method="activateFeed",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="isFeedActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"act3",
        )
    )
    assert result.abi_return is True


def test_create_second_feed(oracle):
    boxes = feed_boxes(1)
    result = oracle.send.call(
        au.AppClientMethodCallParams(
            method="createFeed",
            args=[18],  # 18 decimals
            box_references=boxes,
            note=b"feed2",
        )
    )
    assert result.abi_return == 1
