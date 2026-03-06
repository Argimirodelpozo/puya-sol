"""Uniswap V4 Position — adapted from Position.t.sol"""
import pytest
import algokit_utils as au
from algosdk import encoding
from helpers import to_int64, grouped_call

# Zero Algorand address (32 zero bytes)
ZERO_ADDR = encoding.encode_address(b'\x00' * 32)

@pytest.mark.localnet
def test_calculatePositionKey_deterministic(helper49, orchestrator, algod_client, account):
    """Same input produces same key."""
    r1 = grouped_call(helper49, "Position.calculatePositionKey", [ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32], orchestrator, algod_client, account)
    r2 = grouped_call(helper49, "Position.calculatePositionKey", [ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32], orchestrator, algod_client, account)
    assert r1 == r2

@pytest.mark.localnet
@pytest.mark.xfail(reason="abi.encodePacked keccak256 — overlapping mstore at sub-32-byte offsets not tracked")
def test_calculatePositionKey_different_ticks(helper49, orchestrator, algod_client, account):
    """Different tick ranges produce different keys."""
    r1 = grouped_call(helper49, "Position.calculatePositionKey", [ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32], orchestrator, algod_client, account)
    r2 = grouped_call(helper49, "Position.calculatePositionKey", [ZERO_ADDR, to_int64(-200), 200, b'\x00' * 32], orchestrator, algod_client, account)
    assert r1 != r2
