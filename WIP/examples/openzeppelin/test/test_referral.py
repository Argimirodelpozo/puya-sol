"""
Referral behavioral tests.
Tests registration, referral recording, and reward claiming.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def referrer_key(addr):
    return mapping_box_key("_referrer", encoding.decode_address(addr))


def count_key(addr):
    return mapping_box_key("_referralCount", encoding.decode_address(addr))


def rewards_key(addr):
    return mapping_box_key("_rewards", encoding.decode_address(addr))


def member_key(addr):
    return mapping_box_key("_memberIndex", encoding.decode_address(addr))


def member_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=referrer_key(addr)),
        au.BoxReference(app_id=0, name=count_key(addr)),
        au.BoxReference(app_id=0, name=rewards_key(addr)),
        au.BoxReference(app_id=0, name=member_key(addr)),
    ]


@pytest.fixture(scope="module")
def ref(localnet, account):
    return deploy_contract(localnet, account, "ReferralTest")


def test_deploy(ref):
    assert ref.app_id > 0


def test_admin(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_reward_rate(ref):
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getRewardRate")
    )
    assert result.abi_return == 200


def test_register(ref, account):
    boxes = member_boxes(account.address)
    ref.send.call(
        au.AppClientMethodCallParams(
            method="initMember",
            args=[account.address],
            box_references=boxes,
        )
    )
    # Register with self as referrer (simplified)
    ref.send.call(
        au.AppClientMethodCallParams(
            method="register",
            args=[account.address, account.address],
            box_references=boxes,
        )
    )


def test_member_count(ref):
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getMemberCount")
    )
    assert result.abi_return == 1


def test_referrer(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(
            method="getReferrer",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=referrer_key(account.address))],
        )
    )
    assert result.abi_return == account.address


def test_record_referral(ref, account):
    # reward = 10000 * 200 / 10000 = 200
    ref.send.call(
        au.AppClientMethodCallParams(
            method="recordReferral",
            args=[account.address, 10000],
            box_references=[
                au.BoxReference(app_id=0, name=rewards_key(account.address)),
                au.BoxReference(app_id=0, name=count_key(account.address)),
            ],
        )
    )


def test_referral_count(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(
            method="getReferralCount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=count_key(account.address))],
        )
    )
    assert result.abi_return == 1


def test_rewards(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(
            method="getRewards",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=rewards_key(account.address))],
        )
    )
    assert result.abi_return == 200


def test_total_referrals(ref):
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getTotalReferrals")
    )
    assert result.abi_return == 1


def test_total_rewards(ref):
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getTotalRewards")
    )
    assert result.abi_return == 200


def test_claim_reward(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(
            method="claimReward",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=rewards_key(account.address))],
        )
    )
    assert result.abi_return == 200


def test_rewards_after_claim(ref, account):
    result = ref.send.call(
        au.AppClientMethodCallParams(
            method="getRewards",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=rewards_key(account.address))],
            note=b"rew2",
        )
    )
    assert result.abi_return == 0


def test_set_reward_rate(ref):
    ref.send.call(
        au.AppClientMethodCallParams(
            method="setRewardRate",
            args=[500],  # 5%
        )
    )
    result = ref.send.call(
        au.AppClientMethodCallParams(method="getRewardRate", note=b"rr2")
    )
    assert result.abi_return == 500
