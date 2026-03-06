"""Uniswap V4 PoolIdLibrary — adapted from PoolId.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au


def make_pool_key(currency0=None, currency1=None, fee=3000, tick_spacing=60, hooks=None):
    """Build a PoolKey tuple for ARC4 encoding: (uint8[32], uint8[32], uint64, uint64, uint8[32])"""
    c0 = currency0 if currency0 is not None else [0] * 32
    c1 = currency1 if currency1 is not None else [0] * 32
    h = hooks if hooks is not None else [0] * 32
    return [c0, c1, fee, tick_spacing, h]


@pytest.mark.localnet
def test_toId_deterministic(helper46, orchestrator, algod_client, account):
    """Same input produces same output."""
    key = make_pool_key()
    r1 = grouped_call(helper46, "PoolIdLibrary.toId", [key], orchestrator, algod_client, account)
    r2 = grouped_call(helper46, "PoolIdLibrary.toId", [key], orchestrator, algod_client, account)
    assert r1 == r2

@pytest.mark.localnet
def test_toId_different_inputs(helper46, orchestrator, algod_client, account):
    """Different fees produce different IDs."""
    key1 = make_pool_key(fee=3000)
    key2 = make_pool_key(fee=500)
    r1 = grouped_call(helper46, "PoolIdLibrary.toId", [key1], orchestrator, algod_client, account)
    r2 = grouped_call(helper46, "PoolIdLibrary.toId", [key2], orchestrator, algod_client, account)
    assert r1 != r2
