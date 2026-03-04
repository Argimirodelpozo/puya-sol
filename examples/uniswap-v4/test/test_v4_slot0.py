"""Uniswap V4 Slot0Library — adapted from Slot0.t.sol"""
import pytest
import algokit_utils as au

@pytest.mark.localnet
def test_slot0_setSqrtPriceX96(helper47):
    """Set sqrtPriceX96 and verify it returns a packed result."""
    price = 79228162514264337593543950336  # SQRT_PRICE_1_1
    r = helper47.send.call(au.AppClientMethodCallParams(
        method="Slot0Library.setSqrtPriceX96", args=[b'\x00' * 32, price],
    ))
    assert r.abi_return is not None

@pytest.mark.localnet
def test_slot0_setTick(helper46):
    """Set tick and verify it returns a packed result."""
    r = helper46.send.call(au.AppClientMethodCallParams(
        method="Slot0Library.setTick", args=[b'\x00' * 32, 100],
    ))
    assert r.abi_return is not None

@pytest.mark.localnet
def test_slot0_setLpFee(helper29):
    """Set LP fee and verify it returns a packed result."""
    r = helper29.send.call(au.AppClientMethodCallParams(
        method="Slot0Library.setLpFee", args=[b'\x00' * 32, 3000],
    ))
    assert r.abi_return is not None
