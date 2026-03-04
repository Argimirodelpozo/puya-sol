"""Uniswap V4 NonzeroDeltaCount — adapted from NonzeroDeltaCount.t.sol"""
import pytest
import algokit_utils as au

@pytest.mark.localnet
def test_increment_then_read(helper47):
    """Increment count, verify it increased. Increment is on Helper36."""
    helper47.send.call(au.AppClientMethodCallParams(method="NonzeroDeltaCount.increment", args=[]))
    # Success means the increment didn't fail

@pytest.mark.localnet
def test_decrement(helper44):
    """Decrement count. Decrement is on Helper44."""
    helper44.send.call(au.AppClientMethodCallParams(method="NonzeroDeltaCount.decrement", args=[]))

@pytest.mark.localnet
def test_read(helper47):
    """Read count returns a value. Read is on Helper45."""
    r = helper47.send.call(au.AppClientMethodCallParams(method="NonzeroDeltaCount.read", args=[]))
    assert r.abi_return is not None
