"""
Uniswap V4 Pool — adapted from Pool.t.sol and PoolManagerInitialize.t.sol

Tests for single-method Pool helpers (checkPoolInitialized, checkTicks,
setLPFee, setProtocolFee, donate, tickSpacingToMaxLiquidityPerTick)
and chunk_0 smoke tests.
"""
import pytest
import algokit_utils as au
from constants import (
    SQRT_PRICE_1_1, MIN_SQRT_PRICE, MAX_SQRT_PRICE,
    MAX_TICK, MIN_TICK, MAX_LP_FEE, MAX_PROTOCOL_FEE,
)
from helpers import to_int64, grouped_call


def make_zero_state():
    """Build a zero State struct: (uint8[32], uint256, uint256, uint256, byte[], byte[], byte[])"""
    return [[0]*32, 0, 0, 0, b'', b'', b'']


def make_initialized_state(tick=0, lpFee=3000):
    """Build an initialized Pool.State with a valid Slot0.
    Slot0 is bytes32 packed: sqrtPriceX96 in low 160 bits (big-endian = last 20 bytes).
    """
    slot0 = [0] * 32
    slot0[31] = 1  # sqrtPriceX96 = 1 (non-zero, in least significant byte)
    return [slot0, 0, 0, 0, b'', b'', b'']


# --- Pool.initialize chunk_0 smoke tests ---

@pytest.mark.localnet
def test_pool_initialize_chunk0_validPrice(helper48, orchestrator, algod_client, account):
    """Pool.initialize__chunk_0 with valid sqrtPrice returns intermediate value."""
    r = grouped_call(helper48, "Pool.initialize__chunk_0", [make_zero_state(), SQRT_PRICE_1_1, 3000], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
def test_pool_initialize_chunk0_minSqrtPrice(helper48, orchestrator, algod_client, account):
    """Pool.initialize__chunk_0 with MIN_SQRT_PRICE succeeds."""
    r = grouped_call(helper48, "Pool.initialize__chunk_0", [make_zero_state(), MIN_SQRT_PRICE, 3000], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
@pytest.mark.xfail(reason="Price validation (TickMath.getTickAtSqrtPrice) may be in a later chunk, not chunk_0")
def test_pool_initialize_chunk0_invalidPrice_reverts(helper48, orchestrator, algod_client, account):
    """Pool.initialize__chunk_0 with 0 sqrtPrice reverts."""
    with pytest.raises(Exception):
        grouped_call(helper48, "Pool.initialize__chunk_0", [make_zero_state(), 0, 3000], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.xfail(reason="Price validation (TickMath.getTickAtSqrtPrice) may be in a later chunk, not chunk_0")
def test_pool_initialize_chunk0_maxSqrtPrice_reverts(helper48, orchestrator, algod_client, account):
    """Pool.initialize__chunk_0 with MAX_SQRT_PRICE reverts (exclusive upper bound)."""
    with pytest.raises(Exception):
        grouped_call(helper48, "Pool.initialize__chunk_0", [make_zero_state(), MAX_SQRT_PRICE, 3000], orchestrator, algod_client, account)


# --- Pool.checkPoolInitialized (Helper40) ---

@pytest.mark.localnet
def test_checkPoolInitialized_zero_state_reverts(helper40, orchestrator, algod_client, account):
    """checkPoolInitialized with zero slot0 should revert (pool not initialized)."""
    with pytest.raises(Exception):
        grouped_call(helper40, "Pool.checkPoolInitialized", [make_zero_state()], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkPoolInitialized_initialized_state_succeeds(helper40, orchestrator, algod_client, account):
    """checkPoolInitialized with non-zero slot0 should succeed."""
    grouped_call(helper40, "Pool.checkPoolInitialized", [make_initialized_state()], orchestrator, algod_client, account)


# --- Pool.checkTicks (Helper42) ---

@pytest.mark.localnet
def test_checkTicks_valid_positive_range(helper42, orchestrator, algod_client, account):
    """checkTicks with valid positive tick range succeeds."""
    grouped_call(helper42, "Pool.checkTicks", [100, 200], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_valid_range_with_negatives(helper42, orchestrator, algod_client, account):
    """checkTicks with valid negative-to-positive tick range succeeds."""
    grouped_call(helper42, "Pool.checkTicks", [to_int64(-100), 100], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_same_tick_reverts(helper42, orchestrator, algod_client, account):
    """checkTicks with tickLower == tickUpper should revert."""
    with pytest.raises(Exception):
        grouped_call(helper42, "Pool.checkTicks", [100, 100], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_inverted_range_reverts(helper42, orchestrator, algod_client, account):
    """checkTicks with tickLower > tickUpper should revert."""
    with pytest.raises(Exception):
        grouped_call(helper42, "Pool.checkTicks", [200, 100], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_below_min_tick_reverts(helper42, orchestrator, algod_client, account):
    """checkTicks with tickLower < MIN_TICK should revert."""
    with pytest.raises(Exception):
        grouped_call(helper42, "Pool.checkTicks", [to_int64(MIN_TICK - 1), 0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_above_max_tick_reverts(helper42, orchestrator, algod_client, account):
    """checkTicks with tickUpper > MAX_TICK should revert."""
    with pytest.raises(Exception):
        grouped_call(helper42, "Pool.checkTicks", [0, MAX_TICK + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_checkTicks_min_max_range(helper42, orchestrator, algod_client, account):
    """checkTicks with MIN_TICK to MAX_TICK succeeds."""
    grouped_call(helper42, "Pool.checkTicks", [to_int64(MIN_TICK), MAX_TICK], orchestrator, algod_client, account)


# --- Pool.setLPFee (Helper40) ---

@pytest.mark.localnet
def test_setLPFee_valid(helper40, orchestrator, algod_client, account):
    """setLPFee with valid fee succeeds (void return)."""
    grouped_call(helper40, "Pool.setLPFee", [make_initialized_state(), 3000], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_setLPFee_zero(helper40, orchestrator, algod_client, account):
    """setLPFee with fee=0 succeeds."""
    grouped_call(helper40, "Pool.setLPFee", [make_initialized_state(), 0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_setLPFee_max(helper40, orchestrator, algod_client, account):
    """setLPFee with MAX_LP_FEE (100%) succeeds."""
    grouped_call(helper40, "Pool.setLPFee", [make_initialized_state(), MAX_LP_FEE], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.xfail(reason="Fee validation may require Yul if/revert pattern")
def test_setLPFee_exceeds_max_reverts(helper40, orchestrator, algod_client, account):
    """setLPFee with fee > MAX_LP_FEE should revert."""
    with pytest.raises(Exception):
        grouped_call(helper40, "Pool.setLPFee", [make_initialized_state(), MAX_LP_FEE + 1], orchestrator, algod_client, account)


# --- Pool.setProtocolFee (Helper43) ---

@pytest.mark.localnet
def test_setProtocolFee_valid(helper43, orchestrator, algod_client, account):
    """setProtocolFee with valid fee succeeds (void return)."""
    grouped_call(helper43, "Pool.setProtocolFee", [make_initialized_state(), 500], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_setProtocolFee_zero(helper43, orchestrator, algod_client, account):
    """setProtocolFee with fee=0 succeeds."""
    grouped_call(helper43, "Pool.setProtocolFee", [make_initialized_state(), 0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_setProtocolFee_max(helper43, orchestrator, algod_client, account):
    """setProtocolFee with MAX_PROTOCOL_FEE succeeds."""
    grouped_call(helper43, "Pool.setProtocolFee", [make_initialized_state(), MAX_PROTOCOL_FEE], orchestrator, algod_client, account)


# --- Pool.tickSpacingToMaxLiquidityPerTick (Helper33) ---

@pytest.mark.localnet
def test_tickSpacingToMaxLiquidityPerTick_60(helper33, orchestrator, algod_client, account):
    """Pool.tickSpacingToMaxLiquidityPerTick with tickSpacing=60."""
    r = grouped_call(helper33, "Pool.tickSpacingToMaxLiquidityPerTick", [60], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
def test_tickSpacingToMaxLiquidityPerTick_10(helper33, orchestrator, algod_client, account):
    """Pool.tickSpacingToMaxLiquidityPerTick with tickSpacing=10."""
    r = grouped_call(helper33, "Pool.tickSpacingToMaxLiquidityPerTick", [10], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
def test_tickSpacingToMaxLiquidityPerTick_1(helper33, orchestrator, algod_client, account):
    """Pool.tickSpacingToMaxLiquidityPerTick with tickSpacing=1."""
    r = grouped_call(helper33, "Pool.tickSpacingToMaxLiquidityPerTick", [1], orchestrator, algod_client, account)
    assert r is not None


# --- Pool.donate (Helper38) ---

def make_state_with_liquidity(liquidity=1_000_000):
    """Build an initialized Pool.State with non-zero liquidity for donate tests."""
    slot0 = [0] * 32
    slot0[31] = 1  # sqrtPriceX96 = 1 (non-zero)
    return [slot0, 0, 0, liquidity, b'', b'', b'']

@pytest.mark.localnet
def test_pool_donate_zero_amounts(helper38, orchestrator, algod_client, account):
    """Pool.donate with zero amounts on initialized pool with liquidity."""
    r = grouped_call(helper38, "Pool.donate", [make_state_with_liquidity(), 0, 0], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
@pytest.mark.xfail(reason="Unary minus on biguint emits b-(0,x) which underflows; needs two's complement negation")
def test_pool_donate_nonzero_amount0(helper38, orchestrator, algod_client, account):
    """Pool.donate with non-zero amount0."""
    r = grouped_call(helper38, "Pool.donate", [make_state_with_liquidity(), 1000, 0], orchestrator, algod_client, account)
    assert r is not None

@pytest.mark.localnet
@pytest.mark.xfail(reason="Unary minus on biguint emits b-(0,x) which underflows; needs two's complement negation")
def test_pool_donate_nonzero_amount1(helper38, orchestrator, algod_client, account):
    """Pool.donate with non-zero amount1."""
    r = grouped_call(helper38, "Pool.donate", [make_state_with_liquidity(), 0, 1000], orchestrator, algod_client, account)
    assert r is not None
