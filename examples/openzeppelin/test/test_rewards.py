"""
Rewards behavioral tests.
Tests earning points, redemption, levels, and bonuses.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def points_key(addr):
    return mapping_box_key("_points", encoding.decode_address(addr))


def earned_key(addr):
    return mapping_box_key("_totalEarned", encoding.decode_address(addr))


def spent_key(addr):
    return mapping_box_key("_totalSpent", encoding.decode_address(addr))


def all_reward_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=points_key(addr)),
        au.BoxReference(app_id=0, name=earned_key(addr)),
        au.BoxReference(app_id=0, name=spent_key(addr)),
    ]


@pytest.fixture(scope="module")
def rewards(localnet, account):
    return deploy_contract(localnet, account, "RewardsTest")


def test_deploy(rewards):
    assert rewards.app_id > 0


def test_owner(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_points_per_action(rewards):
    result = rewards.send.call(
        au.AppClientMethodCallParams(method="pointsPerAction")
    )
    assert result.abi_return == 10


def test_earn_points(rewards, account):
    boxes = all_reward_boxes(account.address)
    rewards.send.call(
        au.AppClientMethodCallParams(
            method="earnPoints",
            args=[account.address, 5],  # 5 actions = 50 points
            box_references=boxes,
        )
    )


def test_points_of(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="pointsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=points_key(account.address))],
        )
    )
    assert result.abi_return == 50


def test_level_after_50(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="getLevel",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
        )
    )
    assert result.abi_return == 1  # > 0 points


def test_earn_more(rewards, account):
    boxes = all_reward_boxes(account.address)
    rewards.send.call(
        au.AppClientMethodCallParams(
            method="earnPoints",
            args=[account.address, 10],  # 10 actions = 100 more
            box_references=boxes,
            note=b"earn2",
        )
    )


def test_level_after_150(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="getLevel",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
            note=b"lv2",
        )
    )
    assert result.abi_return == 2  # >= 100


def test_award_bonus(rewards, account):
    boxes = all_reward_boxes(account.address)
    rewards.send.call(
        au.AppClientMethodCallParams(
            method="awardBonus",
            args=[account.address, 500],
            box_references=boxes,
        )
    )


def test_total_earned(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="totalEarnedOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=earned_key(account.address))],
        )
    )
    assert result.abi_return == 650  # 50 + 100 + 500


def test_redeemable_units(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="redeemableUnits",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=points_key(account.address))],
        )
    )
    assert result.abi_return == 6  # 650 / 100 = 6


def test_redeem(rewards, account):
    boxes = all_reward_boxes(account.address)
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="redeem",
            args=[account.address, 3],  # redeem 3 units = 300 points
            box_references=boxes,
        )
    )
    assert result.abi_return == 300


def test_points_after_redeem(rewards, account):
    result = rewards.send.call(
        au.AppClientMethodCallParams(
            method="pointsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=points_key(account.address))],
            note=b"pts2",
        )
    )
    assert result.abi_return == 350  # 650 - 300


def test_total_redeemed(rewards):
    result = rewards.send.call(
        au.AppClientMethodCallParams(method="totalRedeemed")
    )
    assert result.abi_return == 3
