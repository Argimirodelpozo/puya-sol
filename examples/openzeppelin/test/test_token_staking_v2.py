"""
TokenStakingV2 behavioral tests.
Tests staking with reward calculation, lockup periods, and time-based logic.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def staking(localnet, account):
    return deploy_contract(localnet, account, "TokenStakingV2")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


# --- Deploy ---

def test_deploy(staking):
    assert staking.app_id > 0


def test_owner(staking, account):
    result = staking.send.call(au.AppClientMethodCallParams(method="owner"))
    assert result.abi_return == account.address


def test_initial_reward_rate(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="rewardRate"))
    assert result.abi_return == 1


def test_initial_lock_duration(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="lockDuration"))
    assert result.abi_return == 100


def test_initial_total_staked(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="totalStaked"))
    assert result.abi_return == 0


def test_initial_staker_count(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="stakerCount"))
    assert result.abi_return == 0


# --- Stake ---

def test_stake(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    lock_key = mapping_box_key("_lockUntil", addr)
    reward_key = mapping_box_key("_lastRewardTime", addr)
    pending_key = mapping_box_key("_pendingRewards", addr)

    staking.send.call(
        au.AppClientMethodCallParams(
            method="stake",
            args=[1000],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, lock_key),
                box_ref(app_id, reward_key),
                box_ref(app_id, pending_key),
            ],
        )
    )


def test_staked_balance(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="stakedBalance",
            args=[addr],
            box_references=[box_ref(app_id, bal_key)],
        )
    )
    assert result.abi_return == 1000


def test_total_staked(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="totalStaked"))
    assert result.abi_return == 1000


def test_staker_count(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="stakerCount"))
    assert result.abi_return == 1


def test_is_locked(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    lock_key = mapping_box_key("_lockUntil", addr)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="isLocked",
            args=[addr],
            box_references=[box_ref(app_id, lock_key)],
        )
    )
    # Should be locked (just staked, lock is 100 seconds)
    assert result.abi_return is True


# --- Unstake fails when locked ---

def test_unstake_while_locked_fails(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    lock_key = mapping_box_key("_lockUntil", addr)
    reward_key = mapping_box_key("_lastRewardTime", addr)
    pending_key = mapping_box_key("_pendingRewards", addr)

    with pytest.raises(Exception):
        staking.send.call(
            au.AppClientMethodCallParams(
                method="unstake",
                args=[500],
                box_references=[
                    box_ref(app_id, bal_key),
                    box_ref(app_id, lock_key),
                    box_ref(app_id, reward_key),
                    box_ref(app_id, pending_key),
                ],
            )
        )


# --- Stake more ---

def test_stake_more(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    lock_key = mapping_box_key("_lockUntil", addr)
    reward_key = mapping_box_key("_lastRewardTime", addr)
    pending_key = mapping_box_key("_pendingRewards", addr)

    staking.send.call(
        au.AppClientMethodCallParams(
            method="stake",
            args=[500],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, lock_key),
                box_ref(app_id, reward_key),
                box_ref(app_id, pending_key),
            ],
        )
    )


def test_staked_balance_after_more(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    result = staking.send.call(
        au.AppClientMethodCallParams(
            method="stakedBalance",
            args=[addr],
            box_references=[box_ref(app_id, bal_key)],
        )
    )
    assert result.abi_return == 1500


def test_staker_count_unchanged(staking):
    result = staking.send.call(au.AppClientMethodCallParams(method="stakerCount"))
    assert result.abi_return == 1  # Same staker, not a new one


# --- Unstake zero fails ---

def test_stake_zero_fails(staking, account):
    app_id = staking.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_stakedBalances", addr)
    lock_key = mapping_box_key("_lockUntil", addr)
    reward_key = mapping_box_key("_lastRewardTime", addr)
    pending_key = mapping_box_key("_pendingRewards", addr)

    with pytest.raises(Exception):
        staking.send.call(
            au.AppClientMethodCallParams(
                method="stake",
                args=[0],
                box_references=[
                    box_ref(app_id, bal_key),
                    box_ref(app_id, lock_key),
                    box_ref(app_id, reward_key),
                    box_ref(app_id, pending_key),
                ],
            )
        )


# --- Set lock duration ---

def test_set_lock_duration(staking):
    staking.send.call(
        au.AppClientMethodCallParams(
            method="setLockDuration",
            args=[0],  # Remove lock for testing
        )
    )
    result = staking.send.call(au.AppClientMethodCallParams(method="lockDuration"))
    assert result.abi_return == 0


# --- Non-owner cannot set rate ---

def test_non_owner_cannot_set_rate(staking, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=staking.app_spec,
            app_id=staking.app_id,
            default_sender=account2.address,
        )
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="setRewardRate",
                args=[999],
            )
        )
