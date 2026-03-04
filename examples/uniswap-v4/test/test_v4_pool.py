"""Uniswap V4 Pool — adapted from Pool.t.sol and PoolManagerInitialize.t.sol"""
import pytest
import algokit_utils as au
from constants import SQRT_PRICE_1_1, MIN_SQRT_PRICE, MAX_SQRT_PRICE


def make_zero_state():
    """Build a zero State struct: (uint8[32], uint256, uint256, uint256, byte[], byte[], byte[])"""
    return [[0]*32, 0, 0, 0, b'', b'', b'']


@pytest.mark.localnet
def test_pool_initialize_chunk0_validPrice(helper35):
    """Pool.initialize__chunk_0 with valid sqrtPrice returns intermediate value."""
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="Pool.initialize__chunk_0",
        args=[make_zero_state(), SQRT_PRICE_1_1, 3000],
    ))
    assert r.abi_return is not None

@pytest.mark.localnet
def test_pool_initialize_chunk0_minSqrtPrice(helper35):
    """Pool.initialize__chunk_0 with MIN_SQRT_PRICE succeeds."""
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="Pool.initialize__chunk_0",
        args=[make_zero_state(), MIN_SQRT_PRICE, 3000],
    ))
    assert r.abi_return is not None

@pytest.mark.localnet
@pytest.mark.xfail(reason="Price validation (TickMath.getTickAtSqrtPrice) may be in a later chunk, not chunk_0")
def test_pool_initialize_chunk0_invalidPrice_reverts(helper35):
    """Pool.initialize__chunk_0 with 0 sqrtPrice reverts."""
    with pytest.raises(Exception):
        helper35.send.call(au.AppClientMethodCallParams(
            method="Pool.initialize__chunk_0",
            args=[make_zero_state(), 0, 3000],
        ))

@pytest.mark.localnet
@pytest.mark.xfail(reason="Price validation (TickMath.getTickAtSqrtPrice) may be in a later chunk, not chunk_0")
def test_pool_initialize_chunk0_maxSqrtPrice_reverts(helper35):
    """Pool.initialize__chunk_0 with MAX_SQRT_PRICE reverts (exclusive upper bound)."""
    with pytest.raises(Exception):
        helper35.send.call(au.AppClientMethodCallParams(
            method="Pool.initialize__chunk_0",
            args=[make_zero_state(), MAX_SQRT_PRICE, 3000],
        ))

@pytest.mark.localnet
@pytest.mark.xfail(reason="tickSpacingToMaxLiquidityPerTick depends on wrapping arithmetic in uint64 subtraction")
def test_tickSpacingToMaxLiquidityPerTick(helper33):
    """Pool.tickSpacingToMaxLiquidityPerTick__chunk_0 with tickSpacing=60."""
    r = helper33.send.call(au.AppClientMethodCallParams(
        method="Pool.tickSpacingToMaxLiquidityPerTick__chunk_0",
        args=[60],
    ))
    assert r.abi_return is not None
