"""Uniswap V4 Lock — adapted from Lock.t.sol"""
import pytest
import algokit_utils as au

@pytest.mark.localnet
def test_lock_unlock_cycle(helper39, helper47):
    """Lock then unlock completes without error."""
    helper39.send.call(au.AppClientMethodCallParams(method="Lock.lock", args=[]))
    helper47.send.call(au.AppClientMethodCallParams(method="Lock.unlock", args=[]))

@pytest.mark.localnet
def test_isUnlocked_default(helper47):
    """isUnlocked on fresh deploy returns a value (default state)."""
    r = helper47.send.call(au.AppClientMethodCallParams(method="Lock.isUnlocked", args=[]))
    assert r.abi_return is not None
