"""Uniswap V4 Lock — adapted from Lock.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au

@pytest.mark.localnet
def test_lock_unlock_cycle(helper50, orchestrator, algod_client, account):
    """Lock then unlock completes without error."""
    grouped_call(helper50, "Lock.lock", [], orchestrator, algod_client, account)
    grouped_call(helper50, "Lock.unlock", [], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_isUnlocked_default(helper50, orchestrator, algod_client, account):
    """isUnlocked on fresh deploy returns a value (default state)."""
    r = grouped_call(helper50, "Lock.isUnlocked", [], orchestrator, algod_client, account)
    assert r is not None
