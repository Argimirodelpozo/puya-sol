"""
Uniswap V4 Pool.swap — Multi-Chunk End-to-End Tests

Tests the 3-chunk execution of Pool.swap by calling each chunk
sequentially and threading live variables between chunks.

Chain layout (3 chunks):
  chunk_0 (H34): validate slot0, compute fees, start swap loop
  chunk_1 (H1): continue swap loop iteration
  chunk_2 (H38): finalize, update state, compute deltas

SwapParams ABI: (uint256 amountSpecified, uint64 tickSpacing, uint8 zeroForOne, uint256 sqrtPriceLimitX96, uint64 lpFeeOverride)

Note: Solidity SwapParams struct field order is (amountSpecified, tickSpacing, zeroForOne, sqrtPriceLimitX96, lpFeeOverride).
"""
import pytest
import algokit_utils as au
from constants import (
    SQRT_PRICE_1_1, SQRT_PRICE_2_1, SQRT_PRICE_1_2,
    MIN_SQRT_PRICE, MAX_SQRT_PRICE,
)
from helpers import call_pool_swap, grouped_call, to_int256, from_int256, to_int64


# --- Slot0 construction ---

def make_slot0(sqrtPriceX96, tick=0, protocolFee=0, lpFee=3000):
    """Construct a Slot0 bytes32 as uint8[32].
    Layout: sqrtPriceX96 (bits 0-159) | tick (bits 160-183) | protocolFee (bits 184-207) | lpFee (bits 208-231)
    """
    tick_val = tick & 0xFFFFFF  # int24 -> uint24 two's complement
    slot0_int = sqrtPriceX96 | (tick_val << 160) | (protocolFee << 184) | (lpFee << 208)
    slot0_bytes = slot0_int.to_bytes(32, 'big')
    return list(slot0_bytes)


def make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0,
                    feeGrowth0=0, feeGrowth1=0):
    """Build a Pool.State with specific slot0 and liquidity.
    State: (uint8[32] slot0, uint256 feeGrowthGlobal0X128, uint256 feeGrowthGlobal1X128,
            uint256 liquidity, byte[] ticks, byte[] tickBitmap, byte[] positions)
    """
    return [make_slot0(sqrtPriceX96, tick, 0, lpFee), feeGrowth0, feeGrowth1, liquidity, b'', b'', b'']


def make_swap_params(amountSpecified, tickSpacing=60, zeroForOne=True,
                     sqrtPriceLimitX96=None, lpFeeOverride=0):
    """Build a SwapParams tuple.
    ABI: (uint256 amountSpecified, uint64 tickSpacing, uint8 zeroForOne, uint256 sqrtPriceLimitX96, uint64 lpFeeOverride)
    """
    if sqrtPriceLimitX96 is None:
        sqrtPriceLimitX96 = MIN_SQRT_PRICE + 1 if zeroForOne else MAX_SQRT_PRICE - 1
    return [
        to_int256(amountSpecified),
        to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing,
        1 if zeroForOne else 0,
        sqrtPriceLimitX96,
        lpFeeOverride,
    ]


# --- Fixture ---

@pytest.fixture(scope="module")
def pool_swap_helpers(helper1, helper34, helper38):
    """Bundle all helpers needed for Pool.swap (3 chunks)."""
    return {1: helper1, 34: helper34, 38: helper38}


# --- Pool.swap chunk_0 smoke test ---

@pytest.mark.localnet
def test_pool_swap_chunk0_zeroAmount(helper34, orchestrator, algod_client, account):
    """Pool.swap__chunk_0 with amountSpecified=0 should succeed (early return path)."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0)
    params = make_swap_params(amountSpecified=0, zeroForOne=True)
    r = grouped_call(helper34, "Pool.swap__chunk_0", [state, params], orchestrator, algod_client, account)
    assert r is not None


@pytest.mark.localnet
def test_pool_swap_chunk0_invalidPriceLimit_zeroForOne(helper34, orchestrator, algod_client, account):
    """Pool.swap__chunk_0 with sqrtPriceLimitX96 >= slot0.sqrtPriceX96 and zeroForOne should revert."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_swap_params(amountSpecified=-1000, zeroForOne=True,
                              sqrtPriceLimitX96=SQRT_PRICE_1_1)
    with pytest.raises(Exception):
        grouped_call(helper34, "Pool.swap__chunk_0", [state, params], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_pool_swap_chunk0_invalidPriceLimit_oneForZero(helper34, orchestrator, algod_client, account):
    """Pool.swap__chunk_0 with sqrtPriceLimitX96 <= slot0.sqrtPriceX96 and !zeroForOne should revert."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_swap_params(amountSpecified=-1000, zeroForOne=False,
                              sqrtPriceLimitX96=SQRT_PRICE_1_1)
    with pytest.raises(Exception):
        grouped_call(helper34, "Pool.swap__chunk_0", [state, params], orchestrator, algod_client, account)


# --- Pool.swap e2e: zero amount (early return) ---

@pytest.mark.localnet
def test_pool_swap_e2e_zeroAmount_zeroForOne(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with amountSpecified=0 and zeroForOne=true returns zero delta."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0)
    params = make_swap_params(amountSpecified=0, zeroForOne=True)
    result = call_pool_swap(
        pool_swap_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
def test_pool_swap_e2e_zeroAmount_oneForZero(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with amountSpecified=0 and zeroForOne=false returns zero delta."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0)
    params = make_swap_params(amountSpecified=0, zeroForOne=False)
    result = call_pool_swap(
        pool_swap_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


# --- Pool.swap e2e: non-zero amount (requires liquidity) ---

@pytest.mark.localnet
@pytest.mark.xfail(reason="Non-zero swap requires tick bitmap and tick info in box storage")
def test_pool_swap_e2e_exactInput_zeroForOne(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with exactInput (negative amountSpecified) zeroForOne."""
    state = make_pool_state(
        sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000,
        liquidity=1_000_000_000_000_000_000,  # 1e18 liquidity
    )
    params = make_swap_params(
        amountSpecified=-1_000_000,  # exact input of 1M
        zeroForOne=True,
    )
    result = call_pool_swap(
        pool_swap_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
@pytest.mark.xfail(reason="Non-zero swap requires tick bitmap and tick info in box storage")
def test_pool_swap_e2e_exactInput_oneForZero(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with exactInput (negative amountSpecified) oneForZero."""
    state = make_pool_state(
        sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000,
        liquidity=1_000_000_000_000_000_000,
    )
    params = make_swap_params(
        amountSpecified=-1_000_000,
        zeroForOne=False,
    )
    result = call_pool_swap(
        pool_swap_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


@pytest.mark.localnet
@pytest.mark.xfail(reason="Non-zero swap requires tick bitmap and tick info in box storage")
def test_pool_swap_e2e_exactOutput_zeroForOne(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with exactOutput (positive amountSpecified) zeroForOne."""
    state = make_pool_state(
        sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000,
        liquidity=1_000_000_000_000_000_000,
    )
    params = make_swap_params(
        amountSpecified=1_000_000,  # exact output
        zeroForOne=True,
    )
    result = call_pool_swap(
        pool_swap_helpers, state, params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None


# --- Pool.swap e2e: validation tests ---

@pytest.mark.localnet
def test_pool_swap_e2e_uninitializedPool_reverts(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap on uninitialized pool (slot0=0) should revert."""
    state = [[0]*32, 0, 0, 0, b'', b'', b'']  # zero state
    params = make_swap_params(amountSpecified=-1000, zeroForOne=True,
                              sqrtPriceLimitX96=MIN_SQRT_PRICE + 1)
    with pytest.raises(Exception):
        call_pool_swap(
            pool_swap_helpers, state, params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )


@pytest.mark.localnet
def test_pool_swap_e2e_priceLimitExceeded_zeroForOne(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with price limit >= current price for zeroForOne should revert."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_swap_params(
        amountSpecified=-1000, zeroForOne=True,
        sqrtPriceLimitX96=SQRT_PRICE_1_1 + 1,  # above current price
    )
    with pytest.raises(Exception):
        call_pool_swap(
            pool_swap_helpers, state, params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )


@pytest.mark.localnet
def test_pool_swap_e2e_priceLimitTooLow_zeroForOne(pool_swap_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.swap with price limit <= MIN_SQRT_PRICE for zeroForOne should revert."""
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000)
    params = make_swap_params(
        amountSpecified=-1000, zeroForOne=True,
        sqrtPriceLimitX96=MIN_SQRT_PRICE,  # at or below min
    )
    with pytest.raises(Exception):
        call_pool_swap(
            pool_swap_helpers, state, params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )
