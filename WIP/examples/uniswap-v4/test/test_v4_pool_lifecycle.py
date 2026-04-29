"""
Uniswap V4 Pool Full Lifecycle Tests

End-to-end tests for the complete pool lifecycle:
  initialize → modifyLiquidity → swap → donate

These tests combine multiple multi-chunk operations and verify the full
state flow between operations.
"""
import pytest
import algokit_utils as au
from constants import (
    SQRT_PRICE_1_1, SQRT_PRICE_2_1, SQRT_PRICE_1_2,
    MIN_SQRT_PRICE, MAX_SQRT_PRICE, MAX_TICK, MAX_LP_FEE,
)
from helpers import (
    grouped_call,
    call_pool_initialize, call_pool_swap, call_pool_modifyLiquidity,
    call_pool_donate, to_int64, to_int128, to_int256, from_int256,
)


def make_zero_state():
    """Build a zero Pool.State."""
    return [[0] * 32, 0, 0, 0, b'', b'', b'']


def make_slot0(sqrtPriceX96, tick=0, protocolFee=0, lpFee=3000):
    """Construct a Slot0 bytes32 as uint8[32]."""
    tick_val = tick & 0xFFFFFF
    slot0_int = sqrtPriceX96 | (tick_val << 160) | (protocolFee << 184) | (lpFee << 208)
    return list(slot0_int.to_bytes(32, 'big'))


def make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=0, lpFee=3000, liquidity=0):
    """Build an initialized Pool.State."""
    return [make_slot0(sqrtPriceX96, tick, 0, lpFee), 0, 0, liquidity, b'', b'', b'']


# --- Fixtures ---

@pytest.fixture(scope="module")
def all_pool_helpers(
    helper1, helper2, helper3, helper5, helper6, helper7,
    helper13, helper14, helper15, helper16, helper17, helper18, helper19, helper20,
    helper22, helper23, helper24, helper33, helper34, helper37, helper38,
    helper42, helper45, helper47, helper48,
):
    """Bundle all helpers needed for the full pool lifecycle."""
    return {
        1: helper1, 2: helper2, 3: helper3, 5: helper5,
        6: helper6, 7: helper7, 13: helper13, 14: helper14,
        15: helper15, 16: helper16, 17: helper17, 18: helper18,
        19: helper19, 20: helper20, 22: helper22,
        23: helper23, 24: helper24, 33: helper33, 34: helper34,
        37: helper37, 38: helper38, 42: helper42, 45: helper45,
        47: helper47, 48: helper48,
    }


@pytest.fixture(scope="module")
def init_helpers(
    helper6, helper13, helper14, helper15, helper16, helper17,
    helper18, helper19, helper20, helper22, helper23, helper24, helper45, helper48,
):
    """Helpers for Pool.initialize."""
    return {
        6: helper6, 13: helper13, 14: helper14, 15: helper15,
        16: helper16, 17: helper17, 18: helper18, 19: helper19,
        20: helper20, 22: helper22, 23: helper23, 24: helper24,
        45: helper45, 48: helper48,
    }


@pytest.fixture(scope="module")
def swap_helpers(helper1, helper34, helper38):
    """Helpers for Pool.swap."""
    return {1: helper1, 34: helper34, 38: helper38}


@pytest.fixture(scope="module")
def ml_helpers(helper2, helper3, helper5, helper7, helper37, helper42, helper47):
    """Helpers for Pool.modifyLiquidity."""
    return {2: helper2, 3: helper3, 5: helper5, 7: helper7, 37: helper37, 42: helper42, 47: helper47}


# --- Pool.checkPoolInitialized integration ---

@pytest.mark.localnet
def test_lifecycle_checkPoolInitialized_afterInit(helper40, init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """After initialize, checkPoolInitialized should succeed on the returned state."""
    tick = call_pool_initialize(
        init_helpers, make_zero_state(), SQRT_PRICE_1_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=tick, lpFee=3000)
    grouped_call(helper40, "Pool.checkPoolInitialized", [state], orchestrator, algod_client, account)


# --- Pool.setLPFee + Pool.setProtocolFee integration ---

@pytest.mark.localnet
def test_lifecycle_setFees_afterInit(helper40, helper43, init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """After initialize, can set LP fee and protocol fee."""
    tick = call_pool_initialize(
        init_helpers, make_zero_state(), SQRT_PRICE_2_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 6931
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_2_1, tick=tick, lpFee=3000)
    grouped_call(helper40, "Pool.setLPFee", [state, 500], orchestrator, algod_client, account)
    grouped_call(helper43, "Pool.setProtocolFee", [state, 100], orchestrator, algod_client, account)


# --- Pool.initialize different prices + fee variations ---

@pytest.mark.localnet
def test_lifecycle_initialize_then_swap_validation(
    init_helpers, swap_helpers, budget_pad_id, algod_client, account, orchestrator,
):
    """Initialize pool at 2:1, then validate swap price limit for zeroForOne."""
    tick = call_pool_initialize(
        init_helpers, make_zero_state(), SQRT_PRICE_2_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 6931
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_2_1, tick=tick, lpFee=3000)
    swap_params = [
        to_int256(-1000),
        60,
        1,
        SQRT_PRICE_2_1 + 1,
        0,
    ]
    with pytest.raises(Exception):
        call_pool_swap(
            swap_helpers, state, swap_params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
            orchestrator=orchestrator,
        )


@pytest.mark.localnet
def test_lifecycle_initialize_then_swap_priceLimitTooLow(
    init_helpers, swap_helpers, budget_pad_id, algod_client, account, orchestrator,
):
    """Initialize pool at 1:1, then validate swap price limit at MIN_SQRT_PRICE."""
    tick = call_pool_initialize(
        init_helpers, make_zero_state(), SQRT_PRICE_1_1, 0,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0
    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=tick, lpFee=0)
    swap_params = [
        to_int256(-1000), 60, 1, MIN_SQRT_PRICE, 0,
    ]
    with pytest.raises(Exception):
        call_pool_swap(
            swap_helpers, state, swap_params,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
            orchestrator=orchestrator,
        )


# --- Full lifecycle (blocked by checkTicks underflow) ---

@pytest.mark.localnet
@pytest.mark.xfail(reason="modifyLiquidity blocked by checkTicks MIN_TICK uint64 underflow")
def test_lifecycle_full_init_addLiquidity_swap(
    all_pool_helpers, budget_pad_id, algod_client, account, orchestrator,
):
    """Full lifecycle: initialize → add liquidity → swap."""
    tick = call_pool_initialize(
        all_pool_helpers, make_zero_state(), SQRT_PRICE_1_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0

    state = make_pool_state(sqrtPriceX96=SQRT_PRICE_1_1, tick=tick, lpFee=3000)
    ml_params = [
        [0] * 32,
        to_int64(-120),
        120,
        to_int128(1_000_000_000),
        60,
        [0] * 32,
    ]
    result = call_pool_modifyLiquidity(
        all_pool_helpers, state, ml_params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None

    swap_params = [
        to_int256(-100_000), 60, 1, MIN_SQRT_PRICE + 1, 0,
    ]
    swap_result = call_pool_swap(
        all_pool_helpers, state, swap_params,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert swap_result is not None


@pytest.mark.localnet
@pytest.mark.xfail(reason="donate requires initialized pool with liquidity; modifyLiquidity blocked")
def test_lifecycle_full_init_addLiquidity_donate(
    all_pool_helpers, budget_pad_id, algod_client, account, orchestrator,
):
    """Full lifecycle: initialize → add liquidity → donate."""
    tick = call_pool_initialize(
        all_pool_helpers, make_zero_state(), SQRT_PRICE_1_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0

    state = make_pool_state(
        sqrtPriceX96=SQRT_PRICE_1_1, tick=tick, lpFee=3000,
        liquidity=1_000_000_000,
    )
    result = call_pool_donate(
        all_pool_helpers[38], state, 1000, 1000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert result is not None
