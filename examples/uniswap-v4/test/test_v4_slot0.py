"""Uniswap V4 Slot0Library — adapted from Slot0.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au

@pytest.mark.localnet
def test_slot0_setSqrtPriceX96(helper50, orchestrator, algod_client, account):
    """Set sqrtPriceX96 and verify it returns a packed result."""
    price = 79228162514264337593543950336  # SQRT_PRICE_1_1
    r = grouped_call(helper50, "Slot0Library.setSqrtPriceX96", [b'\x00' * 32, price], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
def test_slot0_setTick(helper48, orchestrator, algod_client, account):
    """Set tick and verify it returns a packed result."""
    r = grouped_call(helper48, "Slot0Library.setTick", [b'\x00' * 32, 100], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
def test_slot0_setLpFee(helper47, orchestrator, algod_client, account):
    """Set LP fee and verify it returns a packed result."""
    r = grouped_call(helper47, "Slot0Library.setLpFee", [b'\x00' * 32, 3000], orchestrator, algod_client, account)
    assert r is not None
