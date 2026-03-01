"""
CouponVault behavioral tests.
Tests coupon creation, redemption, and value tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def value_key(cid):
    return mapping_box_key("_couponValue", cid.to_bytes(64, "big"))
def redeemed_key(cid):
    return mapping_box_key("_couponRedeemed", cid.to_bytes(64, "big"))
def expiry_key(cid):
    return mapping_box_key("_couponExpiry", cid.to_bytes(64, "big"))

def coupon_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=value_key(cid)),
        au.BoxReference(app_id=0, name=redeemed_key(cid)),
        au.BoxReference(app_id=0, name=expiry_key(cid)),
    ]

@pytest.fixture(scope="module")
def cv(localnet, account):
    return deploy_contract(localnet, account, "CouponVaultTest")

def test_deploy(cv):
    assert cv.app_id > 0

def test_admin(cv, account):
    r = cv.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_coupon(cv):
    boxes = coupon_boxes(0)
    cv.send.call(au.AppClientMethodCallParams(
        method="initCoupon", args=[0], box_references=boxes))
    r = cv.send.call(au.AppClientMethodCallParams(
        method="createCoupon", args=[50, 9999],
        box_references=boxes))
    assert r.abi_return == 0

def test_coupon_count(cv):
    r = cv.send.call(au.AppClientMethodCallParams(method="getCouponCount"))
    assert r.abi_return == 1

def test_coupon_value(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="getCouponValue", args=[0],
        box_references=[au.BoxReference(app_id=0, name=value_key(0))]))
    assert r.abi_return == 50

def test_coupon_expiry(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="getCouponExpiry", args=[0],
        box_references=[au.BoxReference(app_id=0, name=expiry_key(0))]))
    assert r.abi_return == 9999

def test_not_redeemed_initially(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="isCouponRedeemed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=redeemed_key(0))]))
    assert r.abi_return is False

def test_total_value(cv):
    r = cv.send.call(au.AppClientMethodCallParams(method="getTotalValue"))
    assert r.abi_return == 50

def test_redeem_coupon(cv):
    cv.send.call(au.AppClientMethodCallParams(
        method="redeemCoupon", args=[0],
        box_references=[au.BoxReference(app_id=0, name=redeemed_key(0))]))

def test_is_redeemed(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="isCouponRedeemed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=redeemed_key(0))],
        note=b"r2"))
    assert r.abi_return is True

def test_redeemed_count(cv):
    r = cv.send.call(au.AppClientMethodCallParams(method="getRedeemedCount"))
    assert r.abi_return == 1

def test_create_second(cv):
    boxes = coupon_boxes(1)
    cv.send.call(au.AppClientMethodCallParams(
        method="initCoupon", args=[1], box_references=boxes))
    r = cv.send.call(au.AppClientMethodCallParams(
        method="createCoupon", args=[100, 8888],
        box_references=boxes, note=b"c2"))
    assert r.abi_return == 1

def test_total_value_final(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="getTotalValue", note=b"tv2"))
    assert r.abi_return == 150

def test_coupon_count_final(cv):
    r = cv.send.call(au.AppClientMethodCallParams(
        method="getCouponCount", note=b"cc2"))
    assert r.abi_return == 2
