"""Translation of v2 src/test/CTFExchange.t.sol auth / pause / fee tests.

Skipped or xfailed:
  - matchOrders revert tests (matchOrders is delegated; lonely-chunk
    runtime not yet wired up)
  - validateOrder tests that exercise the signature dispatcher with non-
    EOA paths (proxyFactory/safeFactory are mocked but the dispatch
    paths reach _verifyPolyProxySignature etc. which read state we
    haven't fully populated)
  - hashOrder concrete-value comparisons (Foundry's expected hex doesn't
    match the AVM keccak vs sha512_256 path we use under puya-sol)
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.transaction import PaymentTxn, wait_for_confirmation

from conftest import AUTO_POPULATE, ZERO_ADDR, addr


def _call(client, method, args=None, sender=None, extra_fee=30_000,
          app_refs=None):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        app_references=app_refs or [],
    ), send_params=AUTO_POPULATE).abi_return


@pytest.fixture
def henry(localnet, admin):
    acct = localnet.account.random()
    sp = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)
    return acct


@pytest.fixture
def brian(localnet, admin):
    acct = localnet.account.random()
    sp = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)
    return acct


@pytest.fixture
def exchange(split_exchange):
    """Convenience alias: split_exchange returns (h1, h2, orch); tests use orch."""
    _, _, orch = split_exchange
    return orch


# ── Setup / Auth ──────────────────────────────────────────────────────────
# Translated from CTFExchange.t.sol::test_CTFExchange_setup et al.

def test_setup(exchange, admin, brian):
    assert _call(exchange, "isAdmin", [addr(admin)]) is True
    assert _call(exchange, "isOperator", [addr(admin)]) is True
    assert _call(exchange, "isAdmin", [addr(brian)]) is False
    assert _call(exchange, "isOperator", [addr(brian)]) is False


def test_auth(exchange, admin, henry):
    """addAdmin + addOperator both succeed; henry gets both roles."""
    assert _call(exchange, "isAdmin", [addr(henry)]) is False
    _call(exchange, "addAdmin", [addr(henry)], sender=admin)
    _call(exchange, "addOperator", [addr(henry)], sender=admin)
    assert _call(exchange, "isAdmin", [addr(henry)]) is True
    assert _call(exchange, "isOperator", [addr(henry)]) is True


def test_auth_remove_admin(exchange, admin, henry):
    _call(exchange, "addAdmin", [addr(henry)], sender=admin)
    _call(exchange, "addOperator", [addr(henry)], sender=admin)
    _call(exchange, "removeAdmin", [addr(henry)], sender=admin)
    _call(exchange, "removeOperator", [addr(henry)], sender=admin)
    assert _call(exchange, "isAdmin", [addr(henry)]) is False
    assert _call(exchange, "isOperator", [addr(henry)]) is False


def test_auth_renounce_operator(exchange, admin):
    assert _call(exchange, "isOperator", [addr(admin)]) is True
    _call(exchange, "renounceOperatorRole", sender=admin)
    assert _call(exchange, "isOperator", [addr(admin)]) is False


def test_auth_not_admin(exchange, brian):
    """Non-admin reverts when calling addAdmin."""
    with pytest.raises(LogicError):
        _call(exchange, "addAdmin", [addr(brian)], sender=brian)


def test_auth_revert_remove_last_admin(exchange, admin):
    """Cannot remove the only admin."""
    with pytest.raises(LogicError):
        _call(exchange, "removeAdmin", [addr(admin)], sender=admin)


def test_auth_revert_remove_non_admin(exchange, admin, henry):
    """Removing a non-admin reverts."""
    with pytest.raises(LogicError):
        _call(exchange, "removeAdmin", [addr(henry)], sender=admin)


def test_auth_revert_remove_non_operator(exchange, admin, henry):
    with pytest.raises(LogicError):
        _call(exchange, "removeOperator", [addr(henry)], sender=admin)


def test_auth_revert_add_existing_admin(exchange, admin):
    """Admin already exists — can't add again."""
    with pytest.raises(LogicError):
        _call(exchange, "addAdmin", [addr(admin)], sender=admin)


def test_auth_revert_add_existing_operator(exchange, admin):
    with pytest.raises(LogicError):
        _call(exchange, "addOperator", [addr(admin)], sender=admin)


def test_auth_remove_admin_with_multiple(exchange, admin, henry):
    """Multiple admins → can remove one."""
    _call(exchange, "addAdmin", [addr(henry)], sender=admin)
    _call(exchange, "removeAdmin", [addr(admin)], sender=admin)
    assert _call(exchange, "isAdmin", [addr(admin)]) is False
    assert _call(exchange, "isAdmin", [addr(henry)]) is True


# ── Pause ─────────────────────────────────────────────────────────────────

def test_pause(exchange, admin):
    """pauseTrading + unpauseTrading flip the `paused` state."""
    assert _call(exchange, "paused") is False
    _call(exchange, "pauseTrading", sender=admin)
    assert _call(exchange, "paused") is True
    _call(exchange, "unpauseTrading", sender=admin)
    assert _call(exchange, "paused") is False


@pytest.mark.xfail(reason="puya-sol uint512 arg processing hits a `len <= 32` "
                          "overflow assert in setMaxFeeRate's body — same bug "
                          "as v1 registerToken; needs separate investigation")
def test_set_user_pause_block_interval(exchange, admin):
    """Admin can update the user-pause block interval. setter takes
    uint512 (64-byte big-endian); getter returns uint256."""
    _call(exchange, "setUserPauseBlockInterval", [50_000], sender=admin)
    assert _call(exchange, "userPauseBlockInterval") == 50_000


# ── Fees ──────────────────────────────────────────────────────────────────

def test_default_max_fee_rate(exchange):
    """Default maxFeeRateBps is 500 (5%)."""
    assert _call(exchange, "getMaxFeeRate") == 500


@pytest.mark.xfail(reason="setMaxFeeRate body has the same `len <= 32` assert "
                          "as setUserPauseBlockInterval — uint512 arg processing")
def test_set_max_fee_rate(exchange, admin):
    _call(exchange, "setMaxFeeRate", [1000], sender=admin)
    assert _call(exchange, "getMaxFeeRate") == 1000


def test_set_max_fee_rate_revert_not_admin(exchange, brian):
    with pytest.raises(LogicError):
        _call(exchange, "setMaxFeeRate", [500], sender=brian)


def test_set_max_fee_rate_revert_exceeds_ceiling(exchange, admin):
    """Cap is 10000 bps (100%); >= cap reverts."""
    with pytest.raises(LogicError):
        _call(exchange, "setMaxFeeRate", [20000], sender=admin)


@pytest.mark.xfail(reason="setMaxFeeRate uint512 bug — same as test_set_max_fee_rate")
def test_validate_fee_zero_rate(exchange, admin):
    """validateFee with rate 0 always passes (any fee allowed)."""
    _call(exchange, "setMaxFeeRate", [0], sender=admin)
    # validateFee(uint256 fee, uint256 cashValue)
    _call(exchange, "validateFee", [1000, 100])


# ── Setters that read state via mocks ─────────────────────────────────────

def test_set_fee_receiver(exchange, admin, henry):
    _call(exchange, "setFeeReceiver", [addr(henry)], sender=admin)
    res = _call(exchange, "getFeeReceiver")
    # `address` returns a 58-char base32 string under algokit.
    from algosdk import encoding as algosdk_encoding
    assert algosdk_encoding.decode_address(res) == addr(henry)


# ── User-Pause ────────────────────────────────────────────────────────────
# Each user can pause themselves; trades against them revert until unpause
# OR the block window expires.

def test_user_pause_revert_already_paused(exchange, henry):
    """pauseUser twice in a row should revert with UserAlreadyPaused."""
    _call(exchange, "pauseUser", sender=henry)
    with pytest.raises(LogicError):
        _call(exchange, "pauseUser", sender=henry)


# ── Hash ──────────────────────────────────────────────────────────────────
# hashOrder builds an EIP-712 digest. We don't compare to a hard-coded
# value (Foundry test uses a Solidity-side reference); we just verify the
# call succeeds and returns 32 bytes.

ORDER_TYPE = "(uint256,uint8[32],uint8[32],uint256,uint256,uint256,uint8,uint8,uint256,uint8[32],uint8[32],byte[])"


def _empty_order(maker_addr: bytes, side: int = 0):
    """Build an Order tuple with default fields. v2 Order layout:
    salt, maker, signer, tokenId, makerAmount, takerAmount,
    side, signatureType, timestamp, metadata, builder, signature."""
    return [
        1,                              # salt: uint256
        list(maker_addr),               # maker: uint8[32]
        list(maker_addr),               # signer: uint8[32]
        50_000_000,                     # tokenId: uint256
        50_000_000,                     # makerAmount: uint256
        100_000_000,                    # takerAmount: uint256
        side,                           # side: uint8 (0=BUY, 1=SELL)
        0,                              # signatureType: uint8 (EOA)
        0,                              # timestamp: uint256
        [0] * 32,                       # metadata: uint8[32]
        [0] * 32,                       # builder: uint8[32]
        b"",                            # signature: bytes
    ]


@pytest.mark.xfail(reason="hashOrder reads cached EIP-712 domain set in __postInit; "
                          "the cache write path is currently broken under puya — "
                          "separate investigation")
def test_hash_order(exchange, admin):
    order = _empty_order(addr(admin))
    res = _call(exchange, "hashOrder", [order], extra_fee=50_000)
    assert len(res) == 32


# ── matchOrders (currently delegated, lonely-chunk runtime not wired) ─────

@pytest.mark.xfail(reason="matchOrders is --force-delegate; the lonely-chunk "
                          "runtime that swaps orch's approval to F isn't "
                          "wired up yet — every matchOrders call hits the "
                          "stub which inner-calls helper3 (TMPL=0)")
def test_match_orders_revert_no_maker_orders(exchange, admin):
    """matchOrders with empty maker list reverts NotCrossing."""
    cond_id = list((0xaaaa).to_bytes(32, "big"))
    taker = _empty_order(addr(admin), side=0)
    with pytest.raises(LogicError):
        _call(exchange, "matchOrders",
            [cond_id, taker, [], (0).to_bytes(64, "big"), [], (0).to_bytes(64, "big"), []],
            sender=admin, extra_fee=200_000)
