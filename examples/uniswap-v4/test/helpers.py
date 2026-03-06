"""
Uniswap V4 Test Helpers — encoding utilities and multi-chunk function wrappers.

All helper calls are wrapped in atomic groups with the orchestrator as txn 0.
This satisfies the group validation checks injected by the contract splitter.
"""
import hashlib
import algokit_utils as au
from algosdk.transaction import ApplicationCallTxn, OnComplete, assign_group_id, wait_for_confirmation
from algosdk.abi import Method
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, TransactionWithSigner, ABIResult,
    AccountTransactionSigner,
)
from algosdk import encoding


# --- Two's complement encoding ---

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


# --- Group call utilities ---

def _resolve_method(helper, method_name):
    """Resolve an ABI method from a helper's app spec."""
    for m in helper.app_spec.methods:
        if m.name == method_name:
            return Method.from_signature(
                f"{m.name}({','.join(str(a.type) for a in m.args)}){m.returns.type}"
            )
    raise ValueError(f"Method {method_name} not found in app spec")


def grouped_call(helper, method_name, args, orchestrator, algod, account):
    """
    Call an ABI method on a helper contract within an atomic group.
    Position 0: bare NoOp on orchestrator (authorization stamp).
    Position 1: the actual ABI method call.
    Returns the ABI return value (decoded).
    """
    sp = algod.suggested_params()
    signer = AccountTransactionSigner(account.private_key)
    atc = AtomicTransactionComposer()

    # Position 0: orchestrator __auth__() ABI call
    auth_method = Method.from_signature("__auth__()void")
    atc.add_method_call(
        app_id=orchestrator.app_id,
        method=auth_method,
        sender=account.address,
        sp=sp,
        signer=signer,
    )

    # Position 1: actual method call
    abi_method = _resolve_method(helper, method_name)
    atc.add_method_call(
        app_id=helper.app_id,
        method=abi_method,
        sender=account.address,
        sp=sp,
        signer=signer,
        method_args=args,
    )

    result = atc.execute(algod, 4)
    return result.abi_results[-1].return_value


def call_with_budget(helper, method_name, args, budget_pad_id, algod, account,
                     extra_budget=1, orchestrator=None):
    """
    Call an ABI method with extra opcode budget via atomic group padding.

    Position 0: orchestrator bare NoOp (when provided).
    Positions 1..N: budget pad transactions.
    Last position: the actual ABI method call.

    Returns the ABI return value (decoded).
    """
    abi_method = _resolve_method(helper, method_name)

    sp = algod.suggested_params()
    signer = AccountTransactionSigner(account.private_key)

    atc = AtomicTransactionComposer()

    # Position 0: orchestrator __auth__() ABI call (authorization stamp)
    if orchestrator is not None:
        auth_method = Method.from_signature("__auth__()void")
        atc.add_method_call(
            app_id=orchestrator.app_id,
            method=auth_method,
            sender=account.address,
            sp=sp,
            signer=signer,
        )

    # Budget pad transactions
    for i in range(extra_budget):
        pad_txn = ApplicationCallTxn(
            sender=account.address,
            sp=sp,
            index=budget_pad_id,
            on_complete=OnComplete.NoOpOC,
            note=f"pad{i}".encode(),
        )
        atc.add_transaction(TransactionWithSigner(pad_txn, signer))

    # Main ABI method call
    atc.add_method_call(
        app_id=helper.app_id,
        method=abi_method,
        sender=account.address,
        sp=sp,
        signer=signer,
        method_args=args,
    )

    result = atc.execute(algod, 4)
    return result.abi_results[-1].return_value


# --- Multi-chunk function wrappers ---

def call_getSqrtPriceAtTick(helper31, helper46, tick: int,
                            orchestrator=None, algod=None, account=None):
    """TickMath.getSqrtPriceAtTick (2 chunks): Helper31 -> Helper46."""
    tick_arg = to_int64(tick) if tick < 0 else tick
    if orchestrator and algod and account:
        r0 = grouped_call(helper31, "TickMath.getSqrtPriceAtTick__chunk_0",
                          [tick_arg], orchestrator, algod, account)
        chunk1_args = list(r0) if isinstance(r0, (list, tuple)) else [r0]
        return grouped_call(helper46, "TickMath.getSqrtPriceAtTick__chunk_1",
                            chunk1_args, orchestrator, algod, account)
    # Fallback for non-validated calls (e.g., if orchestrator not available)
    r0 = helper31.send.call(au.AppClientMethodCallParams(
        method="TickMath.getSqrtPriceAtTick__chunk_0", args=[tick_arg]))
    intermediate = r0.abi_return
    chunk1_args = list(intermediate) if isinstance(intermediate, (list, tuple)) else [intermediate]
    r1 = helper46.send.call(au.AppClientMethodCallParams(
        method="TickMath.getSqrtPriceAtTick__chunk_1", args=chunk1_args))
    return r1.abi_return


def call_getTickAtSqrtPrice(helper29, helper27, helper28, sqrtPriceX96: int,
                            helper8=None, orchestrator=None, algod=None, account=None,
                            budget_pad_id=None):
    """TickMath.getTickAtSqrtPrice (4 chunks): Helper29->27->28->8."""
    use_group = orchestrator and algod and account

    def _call(helper, method, args):
        if use_group and budget_pad_id:
            return call_with_budget(helper, method, args, budget_pad_id, algod, account, extra_budget=4, orchestrator=orchestrator)
        if use_group:
            return grouped_call(helper, method, args, orchestrator, algod, account)
        r = helper.send.call(au.AppClientMethodCallParams(method=method, args=args))
        return r.abi_return

    r0 = _call(helper29, "TickMath.getTickAtSqrtPrice__chunk_0", [sqrtPriceX96])
    live = list(r0) if isinstance(r0, (list, tuple)) else [r0]

    r1 = _call(helper27, "TickMath.getTickAtSqrtPrice__chunk_1", [sqrtPriceX96] + live)
    live = list(r1) if isinstance(r1, (list, tuple)) else [r1]

    r2 = _call(helper28, "TickMath.getTickAtSqrtPrice__chunk_2", [sqrtPriceX96] + live)
    if helper8 is None:
        return r2

    live = list(r2) if isinstance(r2, (list, tuple)) else [r2]
    return _call(helper8, "TickMath.getTickAtSqrtPrice__chunk_3", [sqrtPriceX96] + live)


def call_computeSwapStep(helper51, helper9, helper52,
                         sqrtPriceCurrentX96, sqrtPriceTargetX96,
                         liquidity, amountRemaining, feePips,
                         orchestrator=None, algod=None, account=None):
    """SwapMath.computeSwapStep (4 chunks): Helper51 -> Helper9 -> Helper51 -> Helper52."""
    use_group = orchestrator and algod and account
    orig_args = [sqrtPriceCurrentX96, sqrtPriceTargetX96, liquidity, amountRemaining, feePips]

    def _call(helper, method, args):
        if use_group:
            return grouped_call(helper, method, args, orchestrator, algod, account)
        r = helper.send.call(au.AppClientMethodCallParams(method=method, args=args))
        return r.abi_return

    r0 = _call(helper51, "SwapMath.computeSwapStep__chunk_0", orig_args)
    live = list(r0) if isinstance(r0, (list, tuple)) else [r0]

    _call(helper9, "SwapMath.computeSwapStep__chunk_1__chunk_0", orig_args + live)

    r2 = _call(helper51, "SwapMath.computeSwapStep__chunk_1__chunk_1", orig_args + live)
    live = list(r2) if isinstance(r2, (list, tuple)) else [r2]

    return _call(helper52, "SwapMath.computeSwapStep__chunk_2", orig_args + live)


def call_compress(helper45, helper35, tick, tickSpacing,
                  orchestrator=None, algod=None, account=None):
    """TickBitmap.compress (2 chunks): Helper45 -> Helper35."""
    use_group = orchestrator and algod and account
    tick_arg = to_int64(tick) if tick < 0 else tick
    ts_arg = to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing

    def _call(helper, method, args):
        if use_group:
            return grouped_call(helper, method, args, orchestrator, algod, account)
        r = helper.send.call(au.AppClientMethodCallParams(method=method, args=args))
        return r.abi_return

    r0 = _call(helper45, "TickBitmap.compress__chunk_0", [tick_arg, ts_arg])
    live = list(r0) if isinstance(r0, (list, tuple)) else [r0]
    return _call(helper35, "TickBitmap.compress__chunk_1", live)


def call_flipTick(helper31, helper50, tick, tickSpacing,
                  orchestrator=None, algod=None, account=None):
    """TickBitmap.flipTick (2 chunks): Helper31 -> Helper50.
    chunk_0 takes (self, tick, tickSpacing) and returns uint64.
    chunk_1 takes (self, tickSpacing, tick) — note different arg order."""
    use_group = orchestrator and algod and account
    tick_arg = to_int64(tick) if tick < 0 else tick
    ts_arg = to_int64(tickSpacing) if tickSpacing < 0 else tickSpacing
    bitmap_state = b''

    def _call(helper, method, args):
        if use_group:
            return grouped_call(helper, method, args, orchestrator, algod, account)
        helper.send.call(au.AppClientMethodCallParams(method=method, args=args))

    _call(helper31, "TickBitmap.flipTick__chunk_0", [bitmap_state, tick_arg, ts_arg])
    _call(helper50, "TickBitmap.flipTick__chunk_1", [bitmap_state, ts_arg, tick_arg])
    return None


# --- Multi-chunk chain caller ---

def chain_call(helpers, chain, original_args,
               orchestrator=None, algod=None, account=None):
    """
    Execute a multi-chunk function by calling chunks sequentially.

    Each chunk is a tuple: (helper_index, method_name).
    The first chunk is called with original_args.
    Subsequent chunks are called with original_args + live_vars_from_prev.
    If a chunk returns void (None), live_vars are kept from the previous chunk.
    Returns the final chunk's abi_return.
    """
    use_group = orchestrator and algod and account
    live_vars = []
    for i, (helper_idx, method_name) in enumerate(chain):
        args = list(original_args) + live_vars if i > 0 else list(original_args)
        if use_group:
            ret = grouped_call(helpers[helper_idx], method_name, args,
                               orchestrator, algod, account)
        else:
            r = helpers[helper_idx].send.call(au.AppClientMethodCallParams(
                method=method_name, args=args))
            ret = r.abi_return
        if ret is not None:
            live_vars = list(ret) if isinstance(ret, (list, tuple)) else [ret]
    return live_vars[0] if len(live_vars) == 1 else live_vars


def call_pool_initialize(helpers, self_state, sqrtPriceX96, lpFee,
                         budget_pad_id=None, algod=None, account=None,
                         orchestrator=None):
    """
    Pool.initialize -- 13 chunks end-to-end.

    Chain: chunk_0 (H48) -> chunk_1__chunk_0..11 (H24,H23,H22,H16,H17,H14,H13,H15,H19,H18,H20,H6) -> chunk_2 (H45)

    chunk_0 validates the pool isn't initialized and starts tick computation.
    chunk_1 sub-chain computes getTickAtSqrtPrice (sub-chunks 0-10).
    chunk_2 constructs Slot0 and returns the computed tick.
    """
    def _call(helper_idx, method, args, extra_budget=0):
        """Call a chunk method, with optional budget padding."""
        h = helpers[helper_idx]
        if extra_budget > 0 and budget_pad_id and algod and account:
            return call_with_budget(h, method, args, budget_pad_id, algod, account,
                                   extra_budget, orchestrator=orchestrator)
        elif orchestrator and algod and account:
            return grouped_call(h, method, args, orchestrator, algod, account)
        else:
            r = h.send.call(au.AppClientMethodCallParams(method=method, args=args))
            return r.abi_return

    # chunk_0: validate + start (Helper48)
    tick_init = _call(48, "Pool.initialize__chunk_0",
                      [self_state, sqrtPriceX96, lpFee])

    # chunk_1 sub-chain: getTickAtSqrtPrice
    B = 6  # budget pads per chunk call

    base = [self_state, sqrtPriceX96, lpFee, tick_init]
    ret = _call(24, "Pool.initialize__chunk_1__chunk_0", base, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(23, "Pool.initialize__chunk_1__chunk_1", base + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(22, "Pool.initialize__chunk_1__chunk_2", base + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    base_no_tick = [self_state, sqrtPriceX96, lpFee]

    chunk1_remaining = [
        (16, "Pool.initialize__chunk_1__chunk_3"),
        (17, "Pool.initialize__chunk_1__chunk_4"),
        (14, "Pool.initialize__chunk_1__chunk_5"),
        (13, "Pool.initialize__chunk_1__chunk_6"),
        (15, "Pool.initialize__chunk_1__chunk_7"),
        (19, "Pool.initialize__chunk_1__chunk_8"),
        (18, "Pool.initialize__chunk_1__chunk_9"),
    ]
    for helper_idx, method in chunk1_remaining:
        ret = _call(helper_idx, method, base_no_tick + live, extra_budget=B)
        live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(20, "Pool.initialize__chunk_1__chunk_10",
                base_no_tick + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    tick_computed = _call(6, "Pool.initialize__chunk_1__chunk_11",
                         base_no_tick + live, extra_budget=B)

    return _call(45, "Pool.initialize__chunk_2",
                 [self_state, sqrtPriceX96, lpFee, tick_computed], extra_budget=B)


def call_tickSpacingToMaxLiquidityPerTick(helper33, tickSpacing,
                                          orchestrator=None, algod=None, account=None):
    """Pool.tickSpacingToMaxLiquidityPerTick -- single method on Helper33."""
    if orchestrator and algod and account:
        return grouped_call(helper33, "Pool.tickSpacingToMaxLiquidityPerTick",
                            [tickSpacing], orchestrator, algod, account)
    r = helper33.send.call(au.AppClientMethodCallParams(
        method="Pool.tickSpacingToMaxLiquidityPerTick", args=[tickSpacing]))
    return r.abi_return


def call_pool_swap(helpers, self_state, params,
                   budget_pad_id=None, algod=None, account=None,
                   orchestrator=None):
    """
    Pool.swap -- 3 chunks.

    Chain: chunk_0 (Helper34) -> chunk_1 (Helper1) -> chunk_2 (Helper38)
    """
    def _call(helper_idx, method, args, extra_budget=0):
        h = helpers[helper_idx]
        if extra_budget > 0 and budget_pad_id and algod and account:
            return call_with_budget(h, method, args, budget_pad_id, algod, account,
                                   extra_budget, orchestrator=orchestrator)
        elif orchestrator and algod and account:
            return grouped_call(h, method, args, orchestrator, algod, account)
        else:
            r = h.send.call(au.AppClientMethodCallParams(method=method, args=args))
            return r.abi_return

    B = 6
    original = [self_state, params]

    ret = _call(34, "Pool.swap__chunk_0", original, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(1, "Pool.swap__chunk_1", original + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    return _call(38, "Pool.swap__chunk_2", original + live, extra_budget=B)


def call_pool_modifyLiquidity(helpers, self_state, params,
                              budget_pad_id=None, algod=None, account=None,
                              orchestrator=None):
    """
    Pool.modifyLiquidity -- 7 chunks.

    Chain: chunk_0 (H42) -> chunk_1__chunk_0 (H3, void) -> chunk_1__chunk_1 (H37)
           -> chunk_2 (H5) -> chunk_3 (H7) -> chunk_4 (H2) -> chunk_5 (H47)
    """
    def _call(helper_idx, method, args, extra_budget=0):
        h = helpers[helper_idx]
        if extra_budget > 0 and budget_pad_id and algod and account:
            return call_with_budget(h, method, args, budget_pad_id, algod, account,
                                   extra_budget, orchestrator=orchestrator)
        elif orchestrator and algod and account:
            return grouped_call(h, method, args, orchestrator, algod, account)
        else:
            r = h.send.call(au.AppClientMethodCallParams(method=method, args=args))
            return r.abi_return

    B = 6
    original = [self_state, params]

    ret = _call(42, "Pool.modifyLiquidity__chunk_0", original, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    _call(3, "Pool.modifyLiquidity__chunk_1__chunk_0", original + live, extra_budget=B)

    ret = _call(37, "Pool.modifyLiquidity__chunk_1__chunk_1", original + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(5, "Pool.modifyLiquidity__chunk_2", original + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(7, "Pool.modifyLiquidity__chunk_3", original + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    ret = _call(2, "Pool.modifyLiquidity__chunk_4", original + live, extra_budget=B)
    live = list(ret) if isinstance(ret, (list, tuple)) else [ret]

    return _call(47, "Pool.modifyLiquidity__chunk_5", original + live, extra_budget=B)


def call_pool_donate(helper38, self_state, amount0, amount1,
                     budget_pad_id=None, algod=None, account=None,
                     orchestrator=None):
    """Pool.donate -- single method on Helper38."""
    if budget_pad_id and algod and account:
        return call_with_budget(helper38, "Pool.donate",
                                [self_state, amount0, amount1],
                                budget_pad_id, algod, account, 3,
                                orchestrator=orchestrator)
    if orchestrator and algod and account:
        return grouped_call(helper38, "Pool.donate",
                            [self_state, amount0, amount1],
                            orchestrator, algod, account)
    r = helper38.send.call(au.AppClientMethodCallParams(
        method="Pool.donate", args=[self_state, amount0, amount1]))
    return r.abi_return


# --- Box seeding for cross-helper state tests ---

def compute_box_key(mapping_name: str, key_bytes: bytes) -> bytes:
    """Compute deterministic box key: mapping_name + sha256(key_bytes)."""
    key_hash = hashlib.sha256(key_bytes).digest()
    return mapping_name.encode("utf-8") + key_hash
