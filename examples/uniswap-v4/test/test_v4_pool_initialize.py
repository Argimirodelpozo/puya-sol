"""
Uniswap V4 Pool.initialize — Multi-Chunk End-to-End Tests

Tests the full 13-chunk execution of Pool.initialize by calling each chunk
sequentially and threading live variables (return values) between chunks.

Chain layout (13 chunks):
  chunk_0 (H48): validate slot0==0, start tick computation
  chunk_1__chunk_0..4 (H8->H7->H6->H5->H4): getTickAtSqrtPrice computation
  chunk_1__chunk_5..9 (H23->H22->H21->H19->H20): continued computation
  chunk_1__chunk_10 (H10): finalize tick computation
  chunk_2 (H46): construct Slot0, return tick
"""
import pytest
import algokit_utils as au
from constants import (
    SQRT_PRICE_1_1, SQRT_PRICE_1_2, SQRT_PRICE_2_1, SQRT_PRICE_4_1,
    MIN_SQRT_PRICE, MAX_SQRT_PRICE, MAX_TICK,
)
from helpers import call_pool_initialize


def make_zero_state():
    """Build a zero Pool.State: (uint8[32], uint256, uint256, uint256, byte[], byte[], byte[])"""
    return [[0] * 32, 0, 0, 0, b'', b'', b'']


def make_initialized_state():
    """Build a Pool.State with non-zero slot0 (already initialized)."""
    slot0 = [0] * 32
    slot0[0] = 1  # non-zero to indicate already initialized
    return [slot0, 0, 0, 0, b'', b'', b'']


@pytest.fixture(scope="module")
def pool_init_helpers(
    helper6, helper13, helper14, helper15, helper16, helper17,
    helper18, helper19, helper20, helper22, helper23, helper24,
    helper45, helper48,
):
    """Bundle all helpers needed for Pool.initialize (13 chunks)."""
    return {
        6: helper6, 13: helper13, 14: helper14, 15: helper15,
        16: helper16, 17: helper17, 18: helper18, 19: helper19,
        20: helper20, 22: helper22, 23: helper23, 24: helper24,
        45: helper45, 48: helper48,
    }


# --- Multi-chunk Pool.initialize end-to-end tests ----

@pytest.mark.localnet
def test_pool_initialize_e2e_price_1_1(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with sqrtPrice for 1:1 ratio -> tick = 0."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), SQRT_PRICE_1_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0


@pytest.mark.localnet
def test_pool_initialize_e2e_maxTick_minus_1(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with MAX_SQRT_PRICE - 1 -> tick = MAX_TICK - 1."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), MAX_SQRT_PRICE - 1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == MAX_TICK - 1


@pytest.mark.localnet
def test_pool_initialize_e2e_price_2_1(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with sqrtPrice for 2:1 ratio -> tick = 6931."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), SQRT_PRICE_2_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 6931


@pytest.mark.localnet
def test_pool_initialize_e2e_price_4_1(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with sqrtPrice for 4:1 ratio -> tick = 13863."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), SQRT_PRICE_4_1, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 13863


@pytest.mark.localnet
@pytest.mark.xfail(reason="Negative tick: int256 two's complement multiplication exceeds 32-byte biguint limit")
def test_pool_initialize_e2e_min_sqrt_price(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with MIN_SQRT_PRICE should complete."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), MIN_SQRT_PRICE, 3000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick is not None


@pytest.mark.localnet
def test_pool_initialize_e2e_zero_lpfee(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with lpFee=0."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), SQRT_PRICE_1_1, 0,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0


@pytest.mark.localnet
def test_pool_initialize_e2e_max_lpfee(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with lpFee=1000000 (100%)."""
    tick = call_pool_initialize(
        pool_init_helpers, make_zero_state(), SQRT_PRICE_1_1, 1000000,
        budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        orchestrator=orchestrator,
    )
    assert tick == 0


@pytest.mark.localnet
def test_pool_initialize_e2e_invalid_zero_price_reverts(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with sqrtPrice=0 should revert."""
    with pytest.raises(Exception):
        call_pool_initialize(
            pool_init_helpers, make_zero_state(), 0, 3000,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )


@pytest.mark.localnet
def test_pool_initialize_e2e_max_price_reverts(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with MAX_SQRT_PRICE should revert (exclusive upper bound)."""
    with pytest.raises(Exception):
        call_pool_initialize(
            pool_init_helpers, make_zero_state(), MAX_SQRT_PRICE, 3000,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )


@pytest.mark.localnet
def test_pool_initialize_e2e_already_initialized_reverts(pool_init_helpers, budget_pad_id, algod_client, account, orchestrator):
    """Pool.initialize with non-zero slot0 should revert (PoolAlreadyInitialized)."""
    with pytest.raises(Exception):
        call_pool_initialize(
            pool_init_helpers, make_initialized_state(),
            SQRT_PRICE_1_1, 3000,
            budget_pad_id=budget_pad_id, algod=algod_client, account=account,
        )
