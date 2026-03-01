"""
Staking behavioral tests.
Tests staking, unstaking, reward calculation, and reward claiming.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def staked_key(addr):
    return mapping_box_key("_staked", encoding.decode_address(addr))


def time_key(addr):
    return mapping_box_key("_stakeTime", encoding.decode_address(addr))


def rewards_key(addr):
    return mapping_box_key("_rewards", encoding.decode_address(addr))


def all_stake_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=staked_key(addr)),
        au.BoxReference(app_id=0, name=time_key(addr)),
        au.BoxReference(app_id=0, name=rewards_key(addr)),
    ]


@pytest.fixture(scope="module")
def staking(localnet, account):
    return deploy_contract(localnet, account, "StakingTest")


def test_deploy(staking):
    assert staking.app_id > 0


def test_owner(staking, account):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_reward_rate(staking):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="rewardRate")
    )
    assert result.abi_return == 100


def test_initial_total_staked(staking):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="totalStaked")
    )
    assert result.abi_return == 0


def test_stake(staking, account):
    boxes = all_stake_boxes(account.address)
    staking.send.call(
        au.AppClientMethodCallParams(
            method="stake",
            args=[account.address, 1000000, 1000],  # 1M tokens at time=1000
            box_references=boxes,
        )
    )


def test_staked_of(staking, account):
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="stakedOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=staked_key(account.address))],
        )
    )
    assert result.abi_return == 1000000


def test_total_staked_after(staking):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="totalStaked", note=b"ts1")
    )
    assert result.abi_return == 1000000


def test_pending_rewards(staking, account):
    # 1M staked * 100 rate * 100 elapsed / 1000000 = 10000
    boxes = all_stake_boxes(account.address)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="pendingRewards",
            args=[account.address, 1100],  # 100 time units later
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000


def test_claim_rewards(staking, account):
    boxes = all_stake_boxes(account.address)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="claimRewards",
            args=[account.address, 1100],
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000


def test_rewards_after_claim(staking, account):
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="rewardsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=rewards_key(account.address))],
        )
    )
    assert result.abi_return == 0  # claimed, so 0


def test_stake_more(staking, account):
    """Staking more should accumulate pending rewards."""
    boxes = all_stake_boxes(account.address)
    staking.send.call(
        au.AppClientMethodCallParams(
            method="stake",
            args=[account.address, 500000, 1200],  # 500K more at time=1200
            box_references=boxes,
        )
    )


def test_staked_after_more(staking, account):
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="stakedOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=staked_key(account.address))],
            note=b"staked2",
        )
    )
    assert result.abi_return == 1500000


def test_rewards_accumulated(staking, account):
    # Before second stake: 1M * 100 * 100 / 1000000 = 10000 rewards accumulated
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="rewardsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=rewards_key(account.address))],
            note=b"rewards2",
        )
    )
    assert result.abi_return == 10000


def test_unstake(staking, account):
    boxes = all_stake_boxes(account.address)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="unstake",
            args=[account.address, 500000, 1300],  # unstake 500K at time=1300
            box_references=boxes,
        )
    )
    assert result.abi_return == 500000


def test_staked_after_unstake(staking, account):
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="stakedOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=staked_key(account.address))],
            note=b"staked3",
        )
    )
    assert result.abi_return == 1000000


def test_total_staked_after_unstake(staking):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="totalStaked", note=b"ts2")
    )
    assert result.abi_return == 1000000


def test_set_reward_rate(staking):
    staking.send.call(
        au.AppClientMethodCallParams(
            method="setRewardRate",
            args=[200],
        )
    )


def test_reward_rate_updated(staking):
    result = staking.send.call(
        au.AppClientMethodCallParams(method="rewardRate", note=b"rr2")
    )
    assert result.abi_return == 200
