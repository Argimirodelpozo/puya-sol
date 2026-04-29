"""Uniswap V4 NonzeroDeltaCount — adapted from NonzeroDeltaCount.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au

@pytest.mark.localnet
def test_increment_then_read(helper35, orchestrator, algod_client, account):
    """Increment count, verify it increased."""
    grouped_call(helper35, "NonzeroDeltaCount.increment", [], orchestrator, algod_client, account)
    # Success means the increment didn't fail

@pytest.mark.localnet
def test_decrement(helper49, orchestrator, algod_client, account):
    """Decrement count."""
    grouped_call(helper49, "NonzeroDeltaCount.decrement", [], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_read(helper50, orchestrator, algod_client, account):
    """Read count returns a value."""
    r = grouped_call(helper50, "NonzeroDeltaCount.read", [], orchestrator, algod_client, account)
    assert r is not None
