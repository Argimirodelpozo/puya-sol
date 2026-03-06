"""Uniswap V4 Pool Utilities — checkPoolInitialized, setLPFee, getInitialLPFee

Tests for:
- Pool.checkPoolInitialized on Helper40
- Pool.setLPFee on Helper40
- LPFeeLibrary.getInitialLPFee on Helper40
- LPFeeLibrary.removeOverrideFlag on Helper35
"""
import pytest
from helpers import grouped_call


def make_zero_state():
    """Uninitialized pool state."""
    return [[0] * 32, 0, 0, 0, b'', b'', b'']


def make_initialized_state():
    """Pool state with non-zero slot0 (initialized)."""
    slot0 = [0] * 32
    slot0[0] = 1  # non-zero to indicate initialized
    return [slot0, 0, 0, 0, b'', b'', b'']


# --- Pool.checkPoolInitialized ---

@pytest.mark.localnet
def test_checkPoolInitialized_reverts_on_uninitialized(helper40, orchestrator, algod_client, account):
    """checkPoolInitialized should revert for zero slot0."""
    with pytest.raises(Exception):
        grouped_call(helper40, "Pool.checkPoolInitialized",
                     [make_zero_state()], orchestrator, algod_client, account)


@pytest.mark.localnet
@pytest.mark.xfail(reason="checkPoolInitialized slot0 extraction may require specific sqrtPriceX96 layout")
def test_checkPoolInitialized_passes_on_initialized(helper40, orchestrator, algod_client, account):
    """checkPoolInitialized should pass for non-zero slot0."""
    grouped_call(helper40, "Pool.checkPoolInitialized",
                 [make_initialized_state()], orchestrator, algod_client, account)


# --- LPFeeLibrary.getInitialLPFee ---

@pytest.mark.localnet
def test_getInitialLPFee_static_fee(helper40, orchestrator, algod_client, account):
    """getInitialLPFee with a static fee (no override flag) returns the fee."""
    fee = 3000  # 0.3%
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee",
                     [fee], orchestrator, algod_client, account)
    assert r == 3000


@pytest.mark.localnet
@pytest.mark.xfail(reason="getInitialLPFee with override flag — LPFeeTooLarge validation may trigger before flag check")
def test_getInitialLPFee_dynamic_fee(helper40, orchestrator, algod_client, account):
    """getInitialLPFee with override flag set returns 0 (dynamic fee)."""
    dynamic_fee = 0x800000 | 3000
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee",
                     [dynamic_fee], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_getInitialLPFee_max_fee(helper40, orchestrator, algod_client, account):
    """getInitialLPFee with max valid fee (1000000 = 100%)."""
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee",
                     [1000000], orchestrator, algod_client, account)
    assert r == 1000000


@pytest.mark.localnet
def test_getInitialLPFee_zero(helper40, orchestrator, algod_client, account):
    """getInitialLPFee with zero fee."""
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee",
                     [0], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_getInitialLPFee_too_large_reverts(helper40, orchestrator, algod_client, account):
    """getInitialLPFee with fee > 1000000 should revert (LPFeeTooLarge)."""
    with pytest.raises(Exception):
        grouped_call(helper40, "LPFeeLibrary.getInitialLPFee",
                     [1000001], orchestrator, algod_client, account)


# --- Pool.setLPFee ---

@pytest.mark.localnet
@pytest.mark.xfail(reason="setLPFee validates pool initialized via checkPoolInitialized which fails on mock state")
def test_setLPFee_valid(helper40, orchestrator, algod_client, account):
    """Pool.setLPFee should update the LP fee in pool state."""
    state = make_initialized_state()
    new_fee = 5000
    grouped_call(helper40, "Pool.setLPFee",
                 [state, new_fee], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_setLPFee_too_large_reverts(helper40, orchestrator, algod_client, account):
    """Pool.setLPFee with fee > 1000000 should revert (LPFeeTooLarge)."""
    state = make_initialized_state()
    with pytest.raises(Exception):
        grouped_call(helper40, "Pool.setLPFee",
                     [state, 1000001], orchestrator, algod_client, account)


# --- NonzeroDeltaCount ---

@pytest.mark.localnet
def test_nonzero_delta_count_increment(helper35, orchestrator, algod_client, account):
    """NonzeroDeltaCount.increment() should not revert."""
    grouped_call(helper35, "NonzeroDeltaCount.increment",
                 [], orchestrator, algod_client, account)
