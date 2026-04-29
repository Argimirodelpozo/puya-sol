"""
Synthetix StakingRewards — Test Suite
Translated from: https://github.com/Synthetixio/synthetix/blob/develop/test/contracts/StakingRewards.js

Original tests by Synthetix (MIT license).
Translated to Python/pytest for Algorand VM testing via puya-sol.

Tests cover: constructor, staking, withdrawing, earning rewards,
claiming rewards, notifyRewardAmount, reward rate calculations.

Skipped tests (require unsupported features):
- External Rewards Recovery (cross-contract ERC20 calls)
- Pausable tests (Pausable removed from AVM version)
- Function permissions for non-owner (partially tested)
- setRewardsDuration during active period (timing-dependent)

Time simulation: original uses block.timestamp via Ethereum time manipulation.
AVM version uses explicit currentTime parameter for identical math.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def pad64(value: int) -> bytes:
    return value.to_bytes(64, "big")


# Duration: 7 days in seconds (original uses 7 days = 604800)
REWARDS_DURATION = 604800
ONE_E18 = 10**18


@pytest.fixture(scope="module")
def staker1(localnet: au.AlgorandClient) -> SigningAccount:
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def staking_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy StakingRewards with 7-day duration."""
    client = deploy_contract(
        localnet, account, "StakingRewards",
        app_args=[pad64(REWARDS_DURATION)],
        fund_amount=2_000_000,
    )
    return client


# ─── Original: Constructor & Settings ───

@pytest.mark.localnet
def test_constructor_sets_duration(staking_client: au.AppClient) -> None:
    """Original: should set rewardsDuration on constructor"""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getRewardsDuration",
            args=[],
        )
    )
    assert result.abi_return == REWARDS_DURATION


# ─── Original: rewardPerToken() ───

@pytest.mark.localnet
def test_reward_per_token_zero_supply(staking_client: au.AppClient) -> None:
    """Original: should return 0 (when no stakers)"""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="rewardPerToken",
            args=[1000],  # currentTime
        )
    )
    assert result.abi_return == 0


# ─── Original: stake() ───

@pytest.mark.localnet
def test_staking_increases_balance(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: staking increases staking balance"""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    staking_client.send.call(
        au.AppClientMethodCallParams(
            method="stake",
            args=[100 * ONE_E18, 1000],  # stake 100 tokens at time 1000
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )

    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 100 * ONE_E18


@pytest.mark.localnet
def test_total_supply_increases(staking_client: au.AppClient) -> None:
    """Original: total supply increases after staking"""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 100 * ONE_E18


# ─── Original: notifyRewardAmount ───

@pytest.mark.localnet
def test_notify_reward_amount(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: notifyRewardAmount sets rewardRate and periodFinish."""
    app_id = staking_client.app_id
    reward = 1000 * ONE_E18  # 1000 reward tokens
    current_time = 2000

    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    staking_client.send.call(
        au.AppClientMethodCallParams(
            method="notifyRewardAmount",
            args=[reward, current_time],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )

    # rewardRate = reward / duration = 1000e18 / 604800
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getRewardRate",
            args=[],
        )
    )
    expected_rate = reward // REWARDS_DURATION
    assert result.abi_return == expected_rate

    # periodFinish = currentTime + duration
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getPeriodFinish",
            args=[],
        )
    )
    assert result.abi_return == current_time + REWARDS_DURATION


# ─── Original: earned() ───

@pytest.mark.localnet
def test_earned_zero_at_start(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: should be 0 when staking just started"""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    # At time 2000 (same as notifyRewardAmount), no time has passed
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="earned",
            args=[account.address, 2000],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_earned_increases_over_time(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: should be > 0 when staking for some time"""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    # After 1 day (86400 seconds)
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="earned",
            args=[account.address, 2000 + 86400],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )
    # Expected: ~142.86 tokens (1000/7 days * 1 day)
    # rewardRate * timeDelta * 1e18 / totalSupply * balance / 1e18
    # = (1000e18 / 604800) * 86400 * 1e18 / (100e18) * (100e18) / 1e18
    # ≈ 142857142857142857142 (in wei)
    earned = result.abi_return
    assert earned > 0
    # Should be approximately 1000/7 * 1e18 ≈ 142.857e18
    expected_approx = (1000 * ONE_E18 * 86400) // REWARDS_DURATION
    # Allow small rounding difference
    assert abs(earned - expected_approx) < ONE_E18


# ─── Original: getReward() ───

@pytest.mark.localnet
def test_get_reward(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: should increase rewards token balance.
    Claim rewards after half the period."""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    half_period = 2000 + REWARDS_DURATION // 2

    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getReward",
            args=[half_period],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )
    reward_claimed = result.abi_return
    # Should be approximately half of 1000 tokens ≈ 500e18
    expected_approx = 500 * ONE_E18
    assert reward_claimed > 0
    assert abs(reward_claimed - expected_approx) < ONE_E18


# ─── Original: withdraw() ───

@pytest.mark.localnet
def test_withdraw(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: should decrease staking balance"""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    # Withdraw 50 tokens at time after period
    end_time = 2000 + REWARDS_DURATION + 100

    staking_client.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[50 * ONE_E18, end_time],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )

    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 50 * ONE_E18  # 100 - 50 remaining

    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 50 * ONE_E18


# ─── Original: claim remaining rewards after period ───

@pytest.mark.localnet
def test_claim_remaining_rewards(
    staking_client: au.AppClient, account: SigningAccount
) -> None:
    """Claim remaining rewards after full period."""
    app_id = staking_client.app_id
    bal_box = mapping_box_key("_balances", addr_bytes(account.address))
    rewards_box = mapping_box_key("rewards", addr_bytes(account.address))
    paid_box = mapping_box_key("userRewardPerTokenPaid", addr_bytes(account.address))

    end_time = 2000 + REWARDS_DURATION + 200

    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getReward",
            args=[end_time],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, rewards_box),
                box_ref(app_id, paid_box),
            ],
        )
    )
    # Should get the remaining ~500 tokens
    remaining = result.abi_return
    assert remaining > 0


# ─── Original: getRewardForDuration ───

@pytest.mark.localnet
def test_get_reward_for_duration(staking_client: au.AppClient) -> None:
    """Original: rewardRate * rewardsDuration should equal total reward."""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getRewardForDuration",
            args=[],
        )
    )
    reward_for_duration = result.abi_return
    # rewardRate * duration ≈ 1000e18 (minus rounding)
    expected = 1000 * ONE_E18
    # Integer division means slight undershoot
    assert abs(reward_for_duration - expected) < ONE_E18


# ─── Original: rewardPerToken should be > 0 after staking + time ───

@pytest.mark.localnet
def test_reward_per_token_positive(staking_client: au.AppClient) -> None:
    """Original: should be > 0 after rewards distributed"""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getRewardPerTokenStored",
            args=[],
        )
    )
    assert result.abi_return > 0


# ─── Original: lastTimeRewardApplicable ───

@pytest.mark.localnet
def test_last_time_reward_applicable(staking_client: au.AppClient) -> None:
    """Original: should return periodFinish when past end."""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="getPeriodFinish",
            args=[],
        )
    )
    period_finish = result.abi_return

    # When currentTime > periodFinish, should return periodFinish
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="lastTimeRewardApplicable",
            args=[period_finish + 1000],
        )
    )
    assert result.abi_return == period_finish


@pytest.mark.localnet
def test_last_time_reward_applicable_during(staking_client: au.AppClient) -> None:
    """Original: should equal current timestamp (when within period)."""
    result = staking_client.send.call(
        au.AppClientMethodCallParams(
            method="lastTimeRewardApplicable",
            args=[3000],  # During the reward period
        )
    )
    assert result.abi_return == 3000
