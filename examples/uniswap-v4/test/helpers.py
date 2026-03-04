"""
Uniswap V4 Test Helpers — encoding utilities and multi-chunk function wrappers.
"""
import hashlib
import algokit_utils as au


# ─── Two's complement encoding ───────────────────────────────────────────────

def to_int64(x: int) -> int:
    """Convert signed int to uint64 two's complement representation."""
    if x >= 0:
        return x
    return (1 << 64) + x


def to_int128(x: int) -> int:
    """Convert signed int128 to uint256 two's complement representation."""
    if x >= 0:
        return x
    return (1 << 128) + x


def to_int256(x: int) -> int:
    """Convert signed int256 to uint256 two's complement representation."""
    if x >= 0:
        return x
    return (1 << 256) + x


def from_int128(x: int) -> int:
    """Convert uint256 two's complement back to signed int128."""
    if x >= (1 << 127):
        return x - (1 << 128)
    return x


def from_int256(x: int) -> int:
    """Convert uint256 two's complement back to signed int256."""
    if x >= (1 << 255):
        return x - (1 << 256)
    return x


def pack_balance_delta(amount0: int, amount1: int) -> int:
    """Pack two int128 amounts into a uint256 BalanceDelta."""
    return (to_int128(amount0) << 128) | to_int128(amount1)


# ─── Multi-chunk function wrappers ────────────────────────────────────────────

def call_getSqrtPriceAtTick(helper34, helper40, tick: int):
    """TickMath.getSqrtPriceAtTick (2 chunks): Helper34 → Helper40."""
    tick_arg = to_int64(tick) if tick < 0 else tick
    r0 = helper34.send.call(au.AppClientMethodCallParams(
        method="TickMath.getSqrtPriceAtTick__chunk_0",
        args=[tick_arg],
    ))
    intermediate = r0.abi_return
    # chunk_1 takes (tick, __free_memory_ptr, absTick, price, sqrtPriceX96)
    chunk1_args = [tick_arg] + (list(intermediate) if isinstance(intermediate, (list, tuple)) else [intermediate])
    r1 = helper40.send.call(au.AppClientMethodCallParams(
        method="TickMath.getSqrtPriceAtTick__chunk_1",
        args=chunk1_args,
    ))
    return r1.abi_return


def call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, sqrtPriceX96: int):
    """TickMath.getTickAtSqrtPrice (6 chunks): Helper25→12→26→28→37→9."""
    r0 = helper25.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_0",
        args=[sqrtPriceX96],
    ))
    # Each subsequent chunk takes sqrtPriceX96 as first arg, then intermediates
    def prepend_orig(intermediate):
        vals = list(intermediate) if isinstance(intermediate, (list, tuple)) else [intermediate]
        return [sqrtPriceX96] + vals
    r1 = helper12.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_1",
        args=prepend_orig(r0.abi_return),
    ))
    r2 = helper26.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_2",
        args=prepend_orig(r1.abi_return),
    ))
    r3 = helper28.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_3",
        args=prepend_orig(r2.abi_return),
    ))
    r4 = helper37.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_4",
        args=prepend_orig(r3.abi_return),
    ))
    r5 = helper9.send.call(au.AppClientMethodCallParams(
        method="TickMath.getTickAtSqrtPrice__chunk_5",
        args=prepend_orig(r4.abi_return),
    ))
    return r5.abi_return


def call_computeSwapStep(helper45, helper8, sqrtPriceCurrentX96, sqrtPriceTargetX96, liquidity, amountRemaining, feePips):
    """SwapMath.computeSwapStep (2 chunks): Helper45 → Helper8."""
    r0 = helper45.send.call(au.AppClientMethodCallParams(
        method="SwapMath.computeSwapStep__chunk_0",
        args=[sqrtPriceCurrentX96, sqrtPriceTargetX96, liquidity, amountRemaining, feePips],
    ))
    # chunk_1 takes all original args + intermediates from chunk_0
    orig_args = [sqrtPriceCurrentX96, sqrtPriceTargetX96, liquidity, amountRemaining, feePips]
    intermediates = list(r0.abi_return) if isinstance(r0.abi_return, (list, tuple)) else [r0.abi_return]
    r1 = helper8.send.call(au.AppClientMethodCallParams(
        method="SwapMath.computeSwapStep__chunk_1",
        args=orig_args + intermediates,
    ))
    return r1.abi_return


def call_compress(helper41, helper6, tick, tickSpacing):
    """TickBitmap.compress (2 chunks): Helper41 → Helper6."""
    tick_arg = to_int64(tick) if tick < 0 else tick
    ts_arg = to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing
    r0 = helper41.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.compress__chunk_0",
        args=[tick_arg, ts_arg],
    ))
    # chunk_1 takes (tick, tickSpacing, compressed) — original args + intermediate
    r1 = helper6.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.compress__chunk_1",
        args=[tick_arg, ts_arg, r0.abi_return],
    ))
    return r1.abi_return


def call_flipTick(helper43, helper41, tick, tickSpacing):
    """TickBitmap.flipTick (2 chunks): Helper43 → Helper41."""
    tick_arg = to_int64(tick) if tick < 0 else tick
    ts_arg = to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing
    r0 = helper43.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.flipTick__chunk_0",
        args=[tick_arg, ts_arg],
    ))
    r1 = helper41.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.flipTick__chunk_1",
        args=list(r0.abi_return) if isinstance(r0.abi_return, (list, tuple)) else [r0.abi_return],
    ))
    return r1.abi_return


# ─── Box seeding for cross-helper state tests ────────────────────────────────

def compute_box_key(mapping_name: str, key_bytes: bytes) -> bytes:
    """Compute deterministic box key: mapping_name + sha256(key_bytes)."""
    key_hash = hashlib.sha256(key_bytes).digest()
    return mapping_name.encode("utf-8") + key_hash
