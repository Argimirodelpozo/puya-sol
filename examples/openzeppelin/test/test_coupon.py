"""
Coupon behavioral tests.
Tests coupon creation, redemption, activation, and deactivation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key
import hashlib


def discount_key(cid):
    return mapping_box_key("_couponDiscount", cid.to_bytes(64, "big"))


def max_uses_key(cid):
    return mapping_box_key("_couponMaxUses", cid.to_bytes(64, "big"))


def use_count_key(cid):
    return mapping_box_key("_couponUseCount", cid.to_bytes(64, "big"))


def active_key(cid):
    return mapping_box_key("_couponActive", cid.to_bytes(64, "big"))


def redeemed_key(addr, cid):
    # keccak256(abi.encodePacked(user, couponId)) — but puya uses sha256 for mapping keys
    # The mapping key is bytes32, computed from keccak256 in Solidity
    # But the box key uses the bytes32 result as the mapping key
    # Actually, the _hasRedeemed mapping is keyed by bytes32
    # The bytes32 key is computed inside the contract via keccak256(abi.encodePacked(user, couponId))
    # For the box key, we need to hash this bytes32 key
    # But wait — in Solidity, the key passed to the mapping is the bytes32 result
    # So box key = "_hasRedeemed" + sha256(bytes32_key)
    # We need to compute the same keccak256 that the contract computes
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(encoding.decode_address(addr) + cid.to_bytes(64, "big"))
    bytes32_key = k.digest()
    return mapping_box_key("_hasRedeemed", bytes32_key)


def coupon_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=discount_key(cid)),
        au.BoxReference(app_id=0, name=max_uses_key(cid)),
        au.BoxReference(app_id=0, name=use_count_key(cid)),
        au.BoxReference(app_id=0, name=active_key(cid)),
    ]


@pytest.fixture(scope="module")
def coupon(localnet, account):
    return deploy_contract(localnet, account, "CouponTest")


def test_deploy(coupon):
    assert coupon.app_id > 0


def test_admin(coupon, account):
    result = coupon.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_initial_count(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(method="getCouponCount")
    )
    assert result.abi_return == 0


def test_create_coupon(coupon, account):
    boxes = coupon_boxes(0)
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="createCoupon",
            args=[500, 10],  # discount=500, maxUses=10
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_coupon_count(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(method="getCouponCount")
    )
    assert result.abi_return == 1


def test_coupon_discount(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="getCouponDiscount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=discount_key(0))],
        )
    )
    assert result.abi_return == 500


def test_coupon_max_uses(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="getCouponMaxUses",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=max_uses_key(0))],
        )
    )
    assert result.abi_return == 10


def test_coupon_active(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="isCouponActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    assert result.abi_return is True


def test_redeem_coupon(coupon, account):
    redeem_box = redeemed_key(account.address, 0)
    # Init redemption box first
    coupon.send.call(
        au.AppClientMethodCallParams(
            method="initRedemption",
            args=[account.address, 0],
            box_references=[au.BoxReference(app_id=0, name=redeem_box)],
        )
    )
    # Redeem
    coupon.send.call(
        au.AppClientMethodCallParams(
            method="redeemCoupon",
            args=[account.address, 0],
            box_references=[
                au.BoxReference(app_id=0, name=redeem_box),
                au.BoxReference(app_id=0, name=use_count_key(0)),
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=max_uses_key(0)),
            ],
        )
    )


def test_use_count_after_redeem(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="getCouponUseCount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=use_count_key(0))],
        )
    )
    assert result.abi_return == 1


def test_has_redeemed(coupon, account):
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="hasRedeemed",
            args=[account.address, 0],
            box_references=[au.BoxReference(app_id=0, name=redeemed_key(account.address, 0))],
        )
    )
    assert result.abi_return is True


def test_total_redemptions(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(method="getTotalRedemptions")
    )
    assert result.abi_return == 1


def test_deactivate_coupon(coupon):
    coupon.send.call(
        au.AppClientMethodCallParams(
            method="deactivateCoupon",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="isCouponActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"act2",
        )
    )
    assert result.abi_return is False


def test_activate_coupon(coupon):
    coupon.send.call(
        au.AppClientMethodCallParams(
            method="activateCoupon",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="isCouponActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"act3",
        )
    )
    assert result.abi_return is True


def test_create_second_coupon(coupon):
    boxes = coupon_boxes(1)
    result = coupon.send.call(
        au.AppClientMethodCallParams(
            method="createCoupon",
            args=[1000, 5],
            box_references=boxes,
            note=b"c2",
        )
    )
    assert result.abi_return == 1


def test_final_count(coupon):
    result = coupon.send.call(
        au.AppClientMethodCallParams(method="getCouponCount", note=b"cnt2")
    )
    assert result.abi_return == 2
