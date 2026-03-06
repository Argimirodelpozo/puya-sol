"""Uniswap V4 Hooks Callbacks — Tests for hook dispatch methods

Tests for individual hook callback methods on Helper43 (before*) and Helper41/11/26 (after*).
These test the hook dispatch logic with a zero/non-hook address (should be no-ops or reverts).
"""
import pytest
from helpers import grouped_call


ZERO_ADDRESS = b'\x00' * 32
ZERO_KEY = ([0] * 32, [0] * 32, 0, 0, [0] * 32)
EMPTY_HOOK_DATA = b''


# --- beforeInitialize (Helper43) ---

@pytest.mark.localnet
def test_beforeInitialize_zero_hooks(helper43, orchestrator, algod_client, account):
    """beforeInitialize with zero address hooks should be a no-op."""
    sqrtPriceX96 = 79228162514264337593543950336  # SQRT_PRICE_1_1
    grouped_call(helper43, "Hooks.beforeInitialize",
                 [ZERO_ADDRESS, ZERO_KEY, sqrtPriceX96],
                 orchestrator, algod_client, account)


# --- afterInitialize (Helper41) ---

@pytest.mark.localnet
def test_afterInitialize_zero_hooks(helper41, orchestrator, algod_client, account):
    """afterInitialize with zero address hooks should be a no-op."""
    sqrtPriceX96 = 79228162514264337593543950336
    tick = 0
    grouped_call(helper41, "Hooks.afterInitialize",
                 [ZERO_ADDRESS, ZERO_KEY, sqrtPriceX96, tick],
                 orchestrator, algod_client, account)


# --- beforeModifyLiquidity (Helper43) ---

@pytest.mark.localnet
def test_beforeModifyLiquidity_zero_hooks(helper43, orchestrator, algod_client, account):
    """beforeModifyLiquidity with zero address hooks should be a no-op."""
    params = (0, 0, 0, [0] * 32)  # (tickLower, tickUpper, liquidityDelta, salt)
    grouped_call(helper43, "Hooks.beforeModifyLiquidity",
                 [ZERO_ADDRESS, ZERO_KEY, params, EMPTY_HOOK_DATA],
                 orchestrator, algod_client, account)


# --- afterModifyLiquidity (Helper26) ---

@pytest.mark.localnet
def test_afterModifyLiquidity_zero_hooks(helper26, orchestrator, algod_client, account):
    """afterModifyLiquidity with zero address hooks should be a no-op."""
    params = (0, 0, 0, [0] * 32)
    delta0 = 0
    delta1 = 0
    r = grouped_call(helper26, "Hooks.afterModifyLiquidity",
                     [ZERO_ADDRESS, ZERO_KEY, params, delta0, delta1, EMPTY_HOOK_DATA],
                     orchestrator, algod_client, account)
    # Returns (callerDelta, hookDelta) as uint256 pair
    assert r is not None


# --- beforeSwap (Helper43) ---

@pytest.mark.localnet
def test_beforeSwap_zero_hooks(helper43, orchestrator, algod_client, account):
    """beforeSwap with zero address hooks should be a no-op."""
    swap_params = (1, 10**18, 0)  # (zeroForOne=true, amountSpecified, sqrtPriceLimitX96)
    r = grouped_call(helper43, "Hooks.beforeSwap",
                     [ZERO_ADDRESS, ZERO_KEY, swap_params, EMPTY_HOOK_DATA],
                     orchestrator, algod_client, account)
    # Returns (amountToSwap, hookReturn, lpFeeOverride) — should pass through
    assert r is not None


# --- afterSwap (Helper11) ---

@pytest.mark.localnet
def test_afterSwap_zero_hooks(helper11, orchestrator, algod_client, account):
    """afterSwap with zero address hooks should be a no-op."""
    swap_params = (1, 10**18, 0)
    swap_delta = 0  # BalanceDelta (int256)
    r = grouped_call(helper11, "Hooks.afterSwap",
                     [ZERO_ADDRESS, ZERO_KEY, swap_params, swap_delta, EMPTY_HOOK_DATA, 0],
                     orchestrator, algod_client, account)
    assert r is not None


# --- beforeDonate (Helper43) ---

@pytest.mark.localnet
def test_beforeDonate_zero_hooks(helper43, orchestrator, algod_client, account):
    """beforeDonate with zero address hooks should be a no-op."""
    amount0 = 1000
    amount1 = 2000
    grouped_call(helper43, "Hooks.beforeDonate",
                 [ZERO_ADDRESS, ZERO_KEY, amount0, amount1, EMPTY_HOOK_DATA],
                 orchestrator, algod_client, account)


# --- afterDonate (Helper43) ---

@pytest.mark.localnet
def test_afterDonate_zero_hooks(helper43, orchestrator, algod_client, account):
    """afterDonate with zero address hooks should be a no-op."""
    amount0 = 1000
    amount1 = 2000
    grouped_call(helper43, "Hooks.afterDonate",
                 [ZERO_ADDRESS, ZERO_KEY, amount0, amount1, EMPTY_HOOK_DATA],
                 orchestrator, algod_client, account)


# --- Helper41: UnsafeMath.add ---

@pytest.mark.localnet
def test_unsafemath_add_via_helper41(helper41, orchestrator, algod_client, account):
    """add(uint256, uint256) on Helper41."""
    r = grouped_call(helper41, "add", [100, 200], orchestrator, algod_client, account)
    assert r == 300


# --- Helper39: UnsafeMath.sub + Hooks.callHook ---

@pytest.mark.localnet
def test_unsafemath_sub_via_helper39(helper39, orchestrator, algod_client, account):
    """sub(uint256, uint256) on Helper39."""
    r = grouped_call(helper39, "sub", [300, 100], orchestrator, algod_client, account)
    assert r == 200


@pytest.mark.localnet
@pytest.mark.xfail(reason="AVM biguint b- underflows on a < b instead of wrapping mod 2^256")
def test_unsafemath_sub_underflow(helper39, orchestrator, algod_client, account):
    """sub(100, 200) should wrap around (unsafe math)."""
    r = grouped_call(helper39, "sub", [100, 200], orchestrator, algod_client, account)
    expected = (1 << 256) - 100
    assert r == expected
