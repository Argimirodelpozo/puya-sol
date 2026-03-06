"""Uniswap V4 Hooks extended — hook callback methods with zero-address hooks.

Tests for:
- Hooks.beforeDonate(address, PoolKey, uint256, uint256, byte[]) -> void  (Helper43)
- Hooks.afterInitialize(address, PoolKey, uint256, uint64) -> void  (Helper41)
- Hooks.beforeModifyLiquidity(address, PoolKey, ModifyLiquidityParams, byte[]) -> void  (Helper43)

PoolKey: (uint8[32], uint8[32], uint64, uint64, uint8[32])
         = (currency0, currency1, fee, tickSpacing, hooks)
ModifyLiquidityParams: (uint64, uint64, uint256, uint8[32])
                     = (tickLower, tickUpper, liquidityDelta, salt)

With zero-address hooks, all callbacks should be no-ops (no external call made).
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from helpers import to_int64, grouped_call
from constants import SQRT_PRICE_1_1


ZERO_ADDRESS = encoding.encode_address(bytes(32))


def make_pool_key(hooks_addr=None):
    """Return a minimal PoolKey tuple with zero addresses."""
    h = hooks_addr if hooks_addr is not None else [0] * 32
    return [[0] * 32, [0] * 32, 0, 0, h]


# ─── beforeDonate tests (Helper43) ──────────────────────────────────────────

@pytest.mark.localnet
def test_beforeDonate_zero_address_noop(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeDonate is a no-op."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.beforeDonate", [ZERO_ADDRESS, key, 0, 0, b''], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_beforeDonate_zero_address_with_amounts(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeDonate with nonzero amounts is still a no-op."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.beforeDonate", [ZERO_ADDRESS, key, 10**18, 10**18, b''], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_beforeDonate_zero_address_with_hookdata(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeDonate with hook data is still a no-op."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.beforeDonate", [ZERO_ADDRESS, key, 100, 200, b'\xde\xad\xbe\xef'], orchestrator, algod_client, account)


# ─── afterInitialize tests (Helper41) ───────────────────────────────────────

@pytest.mark.localnet
def test_afterInitialize_zero_address_noop(helper41, orchestrator, algod_client, account):
    """Zero-address hook for afterInitialize is a no-op."""
    key = make_pool_key()
    grouped_call(helper41, "Hooks.afterInitialize", [ZERO_ADDRESS, key, SQRT_PRICE_1_1, 0], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_afterInitialize_zero_address_with_tick(helper41, orchestrator, algod_client, account):
    """Zero-address hook for afterInitialize with nonzero tick is still a no-op."""
    key = make_pool_key()
    grouped_call(helper41, "Hooks.afterInitialize", [ZERO_ADDRESS, key, SQRT_PRICE_1_1, 100], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_afterInitialize_zero_address_negative_tick(helper41, orchestrator, algod_client, account):
    """Zero-address hook for afterInitialize with negative tick is still a no-op."""
    key = make_pool_key()
    grouped_call(helper41, "Hooks.afterInitialize", [ZERO_ADDRESS, key, SQRT_PRICE_1_1, to_int64(-100)], orchestrator, algod_client, account)


# ─── beforeModifyLiquidity tests (Helper43) ─────────────────────────────────

@pytest.mark.localnet
def test_beforeModifyLiquidity_zero_address_noop(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeModifyLiquidity is a no-op."""
    key = make_pool_key()
    # ModifyLiquidityParams: (tickLower, tickUpper, liquidityDelta, salt)
    params = (0, 100, 0, [0] * 32)
    grouped_call(helper43, "Hooks.beforeModifyLiquidity", [ZERO_ADDRESS, key, params, b''], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_beforeModifyLiquidity_zero_address_with_liquidity(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeModifyLiquidity with nonzero liquidity is still a no-op."""
    key = make_pool_key()
    params = (to_int64(-100), 100, 10**18, [0] * 32)
    grouped_call(helper43, "Hooks.beforeModifyLiquidity", [ZERO_ADDRESS, key, params, b''], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_beforeModifyLiquidity_zero_address_with_hookdata(helper43, orchestrator, algod_client, account):
    """Zero-address hook for beforeModifyLiquidity with hook data is still a no-op."""
    key = make_pool_key()
    params = (0, 200, 5 * 10**17, [0] * 32)
    grouped_call(helper43, "Hooks.beforeModifyLiquidity", [ZERO_ADDRESS, key, params, b'\x01\x02\x03'], orchestrator, algod_client, account)


# ─── afterDonate tests (Helper43) ───────────────────────────────────────────

@pytest.mark.localnet
def test_afterDonate_zero_address_noop(helper43, orchestrator, algod_client, account):
    """Zero-address hook for afterDonate is a no-op."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.afterDonate", [ZERO_ADDRESS, key, 0, 0, b''], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_afterDonate_zero_address_with_amounts(helper43, orchestrator, algod_client, account):
    """Zero-address hook for afterDonate with nonzero amounts is still a no-op."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.afterDonate", [ZERO_ADDRESS, key, 10**18, 10**18, b''], orchestrator, algod_client, account)
