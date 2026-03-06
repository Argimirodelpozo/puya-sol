"""
Uniswap V4 Pool.modifyLiquidity — Multi-Chunk End-to-End Tests

Tests the 7-chunk execution of Pool.modifyLiquidity by calling each chunk
sequentially and threading live variables between chunks.

Chain layout (7 chunks):
  chunk_0 (H42): checkTicks, start modifyLiquidity
  chunk_1__chunk_0 (H3): void-returning intermediate (updateTick logic)
  chunk_1__chunk_1 (H37): continue updateTick/flipTick
  chunk_2 (H5): fee growth inside calculation
  chunk_3 (H7): position update, compute deltas
  chunk_4 (H2): compute balance deltas
  chunk_5 (H47): finalize

ModifyLiquidityParams ABI: (uint8[32] owner, uint64 tickLower, uint64 tickUpper,
                             uint256 liquidityDelta, uint64 tickSpacing, uint8[32] salt)
"""
import pytest
import algokit_utils as au
from constants import SQRT_PRICE_1_1, SQRT_PRICE_2_1, MAX_TICK, MIN_TICK
from helpers import call_pool_modifyLiquidity, grouped_call, to_int64, to_int128, to_int256


# --- Slot0 construction ---

def make_slot0(sqrtPriceX96, tick=0, protocolFee=0, lpFee=3000):
    """Construct a Slot0 bytes32 as uint8[32]."""
    tick_val = tick & 0xFFFFFF
    slot0_int = sqrtPriceX96 | (tick_val << 160) | (protocolFee << 184) | (lpFee << 208)
    slot0_bytes = slot0_int.to_bytes(32, 'big')
    return list(slot0_bytes)


def make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0):
    """Build a Pool.State."""
    return [make_slot0(sqrtPriceX96, tick, 0, lpFee), 0, 0, liquidity, b'', b'', b'']


def make_modify_params(owner=None, tickLower=-60, tickUpper=60,
                       liquidityDelta=0, tickSpacing=60, salt=None):
    """Build ModifyLiquidityParams tuple.
    ABI: (uint8[32], uint64, uint64, uint256, uint64, uint8[32])
    """
    if owner is None:
        owner = [0] * 32
    if salt is None:
        salt = [0] * 32
    return [
        owner,
        to_int64(tickLower) if tickLower < 0 else tickLower,
        to_int64(tickUpper) if tickUpper < 0 else tickUpper,
        to_int128(liquidityDelta),
        to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing,
        salt,
    ]


# --- Fixtures ---

@pytest.fixture(scope="module")
def pool_ml_helpers(helper2, helper3, helper5, helper7, helper37, helper42, helper47):
    """Bundle all helpers needed for Pool.modifyLiquidity (7 chunks)."""
    return {2: helper2, 3: helper3, 5: helper5, 7: helper7, 37: helper37, 42: helper42, 47: helper47}


# --- Pool.modifyLiquidity chunk_0 tests ---

@pytest.mark.localnet
def test_modifyLiquidity_chunk0_validTicks(helper42, orchestrator, algod_client, account):
    """Pool.modifyLiquidity__chunk_0 with valid ticks succeeds."""
    state = make_pool_state()
    params = make_modify_params(tickLower=-60, tickUpper=60, liquidityDelta=0)
    r = grouped_call(helper42, "Pool.modifyLiquidity__chunk_0", [state, params], orchestrator, algod_client, account)
    assert r is not None


@pytest.mark.localnet
def test_modifyLiquidity_chunk0_positiveTicks(helper42, orchestrator, algod_client, account):
    """Pool.modifyLiquidity__chunk_0 with positive-only ticks succeeds."""
    state = make_pool_state()
    params = make_modify_params(tickLower=100, tickUpper=200, liquidityDelta=0)
    r = grouped_call(helper42, "Pool.modifyLiquidity__chunk_0", [state, params], orchestrator, algod_client, account)
    assert r is not None


# --- Pool.modifyLiquidity e2e tests ---

@pytest.mark.localnet
@pytest.mark.xfail(reason="modifyLiquidity e2e requires tick info boxes in storage")
def test_modifyLiquidity_e2e_zeroLiquidityDelta(pool_ml_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.modifyLiquidity with liquidityDelta=0 (no-op liquidity change)."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_modify_params(tickLower=-60, tickUpper=60, liquidityDelta=0)
    result = call_pool_modifyLiquidity(
        pool_ml_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
@pytest.mark.xfail(reason="modifyLiquidity e2e requires tick info boxes in storage")
def test_modifyLiquidity_e2e_addLiquidity(pool_ml_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.modifyLiquidity adding liquidity (positive liquidityDelta)."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_modify_params(
        tickLower=-120, tickUpper=120,
        liquidityDelta=1_000_000_000,
        tickSpacing=60,
    )
    result = call_pool_modifyLiquidity(
        pool_ml_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
@pytest.mark.xfail(reason="modifyLiquidity e2e requires tick info boxes in storage")
def test_modifyLiquidity_e2e_removeLiquidity(pool_ml_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.modifyLiquidity removing liquidity (negative liquidityDelta)."""
    state = make_pool_state(
        sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000,
        liquidity=1_000_000_000,
    )
    params = make_modify_params(
        tickLower=-120, tickUpper=120,
        liquidityDelta=-500_000_000,
        tickSpacing=60,
    )
    result = call_pool_modifyLiquidity(
        pool_ml_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
def test_modifyLiquidity_e2e_invertedTicks_reverts(pool_ml_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.modifyLiquidity with tickLower > tickUpper should revert (checkTicks catches it)."""
    state = make_pool_state()
    params = make_modify_params(tickLower=200, tickUpper=100, liquidityDelta=0)
    with pytest.raises(Exception):
        call_pool_modifyLiquidity(
            pool_ml_helpers, state, params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )


@pytest.mark.localnet
@pytest.mark.xfail(reason="modifyLiquidity e2e requires tick info boxes in storage")
def test_modifyLiquidity_e2e_wideTicks(pool_ml_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.modifyLiquidity with wide tick range."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_modify_params(
        tickLower=-887220, tickUpper=887220,  # near min/max, aligned to tickSpacing=60
        liquidityDelta=1_000_000,
        tickSpacing=60,
    )
    result = call_pool_modifyLiquidity(
        pool_ml_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None
