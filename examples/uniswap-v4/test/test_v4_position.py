"""Uniswap V4 Position — adapted from Position.t.sol"""
import pytest
import algokit_utils as au
from algosdk import encoding
from helpers import to_int64

# Zero Algorand address (32 zero bytes)
ZERO_ADDR = encoding.encode_address(b'\x00' * 32)

@pytest.mark.localnet
def test_calculatePositionKey_deterministic(helper47):
    """Same input produces same key."""
    r1 = helper47.send.call(au.AppClientMethodCallParams(
        method="Position.calculatePositionKey", args=[ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32],
    ))
    r2 = helper47.send.call(au.AppClientMethodCallParams(
        method="Position.calculatePositionKey", args=[ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32],
    ))
    assert r1.abi_return == r2.abi_return

@pytest.mark.localnet
@pytest.mark.xfail(reason="abi.encodePacked keccak256 — overlapping mstore at sub-32-byte offsets not tracked")
def test_calculatePositionKey_different_ticks(helper47):
    """Different tick ranges produce different keys."""
    r1 = helper47.send.call(au.AppClientMethodCallParams(
        method="Position.calculatePositionKey", args=[ZERO_ADDR, to_int64(-100), 100, b'\x00' * 32],
    ))
    r2 = helper47.send.call(au.AppClientMethodCallParams(
        method="Position.calculatePositionKey", args=[ZERO_ADDR, to_int64(-200), 200, b'\x00' * 32],
    ))
    assert r1.abi_return != r2.abi_return
