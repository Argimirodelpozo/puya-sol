"""
Exchange behavioral tests.
Tests order placement, filling, cancellation, and volume tracking.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def owner_key(oid):
    return mapping_box_key("_orderOwner", oid.to_bytes(64, "big"))

def price_key(oid):
    return mapping_box_key("_orderPrice", oid.to_bytes(64, "big"))

def amount_key(oid):
    return mapping_box_key("_orderAmount", oid.to_bytes(64, "big"))

def is_buy_key(oid):
    return mapping_box_key("_orderIsBuy", oid.to_bytes(64, "big"))

def status_key(oid):
    return mapping_box_key("_orderStatus", oid.to_bytes(64, "big"))

def order_boxes(oid):
    return [
        au.BoxReference(app_id=0, name=owner_key(oid)),
        au.BoxReference(app_id=0, name=price_key(oid)),
        au.BoxReference(app_id=0, name=amount_key(oid)),
        au.BoxReference(app_id=0, name=is_buy_key(oid)),
        au.BoxReference(app_id=0, name=status_key(oid)),
    ]


@pytest.fixture(scope="module")
def exc(localnet, account):
    return deploy_contract(localnet, account, "ExchangeTest")


def test_deploy(exc):
    assert exc.app_id > 0

def test_admin(exc, account):
    r = exc.send.call(au.AppClientMethodCallParams(method="admin"))
    assert r.abi_return == account.address

def test_place_buy_order(exc, account):
    boxes = order_boxes(0)
    exc.send.call(au.AppClientMethodCallParams(
        method="initOrder", args=[0], box_references=boxes))
    r = exc.send.call(au.AppClientMethodCallParams(
        method="placeOrder", args=[account.address, 50, 100, True],
        box_references=boxes))
    assert r.abi_return == 0

def test_order_count(exc):
    r = exc.send.call(au.AppClientMethodCallParams(method="orderCount"))
    assert r.abi_return == 1

def test_order_owner(exc, account):
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    assert r.abi_return == account.address

def test_order_price(exc):
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderPrice", args=[0],
        box_references=[au.BoxReference(app_id=0, name=price_key(0))]))
    assert r.abi_return == 50

def test_order_is_buy(exc):
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderIsBuy", args=[0],
        box_references=[au.BoxReference(app_id=0, name=is_buy_key(0))]))
    assert r.abi_return is True

def test_order_value(exc):
    # 50 * 100 = 5000
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderValue", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=price_key(0)),
            au.BoxReference(app_id=0, name=amount_key(0)),
        ]))
    assert r.abi_return == 5000

def test_total_volume(exc):
    r = exc.send.call(au.AppClientMethodCallParams(method="totalVolume"))
    assert r.abi_return == 5000

def test_fill_order(exc):
    exc.send.call(au.AppClientMethodCallParams(
        method="fillOrder", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))

def test_order_status_filled(exc):
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderStatus", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return == 1

def test_total_filled(exc):
    r = exc.send.call(au.AppClientMethodCallParams(method="totalFilled"))
    assert r.abi_return == 1

def test_place_sell_order(exc, account):
    boxes = order_boxes(1)
    exc.send.call(au.AppClientMethodCallParams(
        method="initOrder", args=[1], box_references=boxes))
    exc.send.call(au.AppClientMethodCallParams(
        method="placeOrder", args=[account.address, 60, 50, False],
        box_references=boxes, note=b"o2"))

def test_cancel_order(exc):
    exc.send.call(au.AppClientMethodCallParams(
        method="cancelOrder", args=[1],
        box_references=[au.BoxReference(app_id=0, name=status_key(1))]))

def test_order_status_cancelled(exc):
    r = exc.send.call(au.AppClientMethodCallParams(
        method="getOrderStatus", args=[1],
        box_references=[au.BoxReference(app_id=0, name=status_key(1))]))
    assert r.abi_return == 2

def test_total_cancelled(exc):
    r = exc.send.call(au.AppClientMethodCallParams(method="totalCancelled"))
    assert r.abi_return == 1
