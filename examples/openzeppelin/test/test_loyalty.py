"""
Loyalty behavioral tests.
Tests enrollment, point earning/redemption, and tier system.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def points_key(addr):
    return mapping_box_key("_points", encoding.decode_address(addr))


def earned_key(addr):
    return mapping_box_key("_totalEarned", encoding.decode_address(addr))


def member_key(addr):
    return mapping_box_key("_memberIndex", encoding.decode_address(addr))


def enrolled_key(addr):
    return mapping_box_key("_isEnrolled", encoding.decode_address(addr))


def member_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=points_key(addr)),
        au.BoxReference(app_id=0, name=earned_key(addr)),
        au.BoxReference(app_id=0, name=member_key(addr)),
        au.BoxReference(app_id=0, name=enrolled_key(addr)),
    ]


@pytest.fixture(scope="module")
def loyalty(localnet, account):
    return deploy_contract(localnet, account, "LoyaltyTest")


def test_deploy(loyalty):
    assert loyalty.app_id > 0


def test_admin(loyalty, account):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_points_rate(loyalty):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getPointsRate")
    )
    assert result.abi_return == 10


def test_init_and_enroll(loyalty, account):
    boxes = member_boxes(account.address)
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="initMember",
            args=[account.address],
            box_references=boxes,
        )
    )
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="enroll",
            args=[account.address],
            box_references=boxes,
        )
    )


def test_is_enrolled(loyalty, account):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="isEnrolled",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=enrolled_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_member_count(loyalty):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getMemberCount")
    )
    assert result.abi_return == 1


def test_initial_tier(loyalty, account):
    # 0 total earned → Bronze (0)
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
        )
    )
    assert result.abi_return == 0


def test_earn_points(loyalty, account):
    # purchase 50 units * 10 rate = 500 points
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="earnPoints",
            args=[account.address, 50],
            box_references=[
                au.BoxReference(app_id=0, name=points_key(account.address)),
                au.BoxReference(app_id=0, name=earned_key(account.address)),
                au.BoxReference(app_id=0, name=enrolled_key(account.address)),
            ],
        )
    )


def test_points_after_earn(loyalty, account):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getPoints",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=points_key(account.address))],
        )
    )
    assert result.abi_return == 500


def test_total_earned(loyalty, account):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getTotalEarned",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
        )
    )
    assert result.abi_return == 500


def test_tier_bronze(loyalty, account):
    # 500 earned → Bronze (0-999)
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
            note=b"t2",
        )
    )
    assert result.abi_return == 0


def test_earn_more_silver(loyalty, account):
    # purchase 80 * 10 = 800 more → total earned = 1300 → Silver
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="earnPoints",
            args=[account.address, 80],
            box_references=[
                au.BoxReference(app_id=0, name=points_key(account.address)),
                au.BoxReference(app_id=0, name=earned_key(account.address)),
                au.BoxReference(app_id=0, name=enrolled_key(account.address)),
            ],
            note=b"ep2",
        )
    )


def test_tier_silver(loyalty, account):
    # 1300 earned → Silver (1000-4999)
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
            note=b"t3",
        )
    )
    assert result.abi_return == 1


def test_redeem_points(loyalty, account):
    # Current points = 1300, redeem 200
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="redeemPoints",
            args=[account.address, 200],
            box_references=[
                au.BoxReference(app_id=0, name=points_key(account.address)),
                au.BoxReference(app_id=0, name=enrolled_key(account.address)),
            ],
        )
    )
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getPoints",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=points_key(account.address))],
            note=b"p2",
        )
    )
    assert result.abi_return == 1100


def test_total_points_issued(loyalty):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getTotalPointsIssued")
    )
    assert result.abi_return == 1300


def test_total_points_redeemed(loyalty):
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getTotalPointsRedeemed")
    )
    assert result.abi_return == 200


def test_earn_gold(loyalty, account):
    # purchase 500 * 10 = 5000 more → total earned = 6300 → Gold
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="earnPoints",
            args=[account.address, 500],
            box_references=[
                au.BoxReference(app_id=0, name=points_key(account.address)),
                au.BoxReference(app_id=0, name=earned_key(account.address)),
                au.BoxReference(app_id=0, name=enrolled_key(account.address)),
            ],
            note=b"ep3",
        )
    )


def test_tier_gold(loyalty, account):
    # 6300 earned → Gold (5000+)
    result = loyalty.send.call(
        au.AppClientMethodCallParams(
            method="getTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
            note=b"t4",
        )
    )
    assert result.abi_return == 2


def test_set_points_rate(loyalty):
    loyalty.send.call(
        au.AppClientMethodCallParams(
            method="setPointsRate",
            args=[20],
        )
    )
    result = loyalty.send.call(
        au.AppClientMethodCallParams(method="getPointsRate", note=b"pr2")
    )
    assert result.abi_return == 20
