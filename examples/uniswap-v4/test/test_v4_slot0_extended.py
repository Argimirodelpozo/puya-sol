"""Uniswap V4 Slot0Library — extended tests for remaining untested methods.

Covers:
  - Slot0Library.tick          (Helper47)
  - Slot0Library.sqrtPriceX96  (Helper47)
  - Slot0Library.setProtocolFee(Helper47)
  - Slot0Library.protocolFee   (Helper49)
  - Slot0Library.lpFee         (Helper34)
  - equals                     (Helper34) — CurrencyLibrary.equals(address, address)
  - greaterThanOrEqualTo       (Helper38) — CurrencyLibrary.greaterThanOrEqualTo(address, address)
  - neq                        (Helper38) — neq(uint256, uint256)

Slot0 packed byte[32] layout (big-endian):
  bits   0-159  (bytes 12-31): sqrtPriceX96  (uint160)
  bits 160-183  (bytes  9-11): tick           (int24,  two's complement)
  bits 184-207  (bytes  6- 8): protocolFee    (uint24)
  bits 208-231  (bytes  3- 5): lpFee          (uint24)
  bits 232-255  (bytes  0- 2): reserved / zero
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from helpers import to_int64, grouped_call


# ---------------------------------------------------------------------------
# Slot0 construction helpers
# ---------------------------------------------------------------------------

def make_slot0(sqrtPriceX96: int = 0, tick: int = 0,
               protocolFee: int = 0, lpFee: int = 0) -> bytes:
    """Pack fields into a 32-byte Slot0 value (big-endian).

    tick is stored as int24 two's complement in bits 160-183.
    Returns a bytes object suitable for use as a byte[32] ABI argument.
    """
    tick_val = tick & 0xFFFFFF  # two's complement 24-bit
    val = (sqrtPriceX96
           | (tick_val << 160)
           | (protocolFee << 184)
           | (lpFee << 208))
    return val.to_bytes(32, 'big')


def slot0_as_list(sqrtPriceX96: int = 0, tick: int = 0,
                  protocolFee: int = 0, lpFee: int = 0):
    """Return a list[int] representation (as algokit returns byte arrays)."""
    return list(make_slot0(sqrtPriceX96, tick, protocolFee, lpFee))


# Well-known constants
SQRT_PRICE_1_1 = 79228162514264337593543950336   # 2**96 — price 1:1
SQRT_PRICE_2_1 = 112045541949572369                # approx sqrt(2) * 2**96
MAX_SQRT_PRICE = (1 << 160) - 1                    # max uint160
MAX_TICK       = 887272                            # TickMath.MAX_TICK
MIN_TICK       = -887272                           # TickMath.MIN_TICK

# Address constants (Currency is address in Uniswap V4)
ZERO_ADDR = encoding.encode_address(b'\x00' * 32)
ADDR_A    = encoding.encode_address(b'\x00' * 31 + b'\x01')
ADDR_B    = encoding.encode_address(b'\x00' * 31 + b'\x02')
ADDR_FF   = encoding.encode_address(b'\xff' * 32)


# ===========================================================================
# Slot0Library.tick (Helper47)
# ===========================================================================

@pytest.mark.localnet
def test_slot0_tick_zero(helper47, orchestrator, algod_client, account):
    """Zero-packed Slot0 yields tick == 0."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=0)
    r = grouped_call(helper47, "Slot0Library.tick", [packed], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_slot0_tick_positive(helper47, orchestrator, algod_client, account):
    """Positive tick value is stored and retrieved correctly."""
    tick = 12345
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=tick)
    r = grouped_call(helper47, "Slot0Library.tick", [packed], orchestrator, algod_client, account)
    # Returned as uint64 — positive tick needs no conversion
    assert r == tick


@pytest.mark.localnet
def test_slot0_tick_negative(helper47, orchestrator, algod_client, account):
    """Negative tick is stored as int24 two's complement and retrieved as uint64."""
    tick = -100
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=tick)
    r = grouped_call(helper47, "Slot0Library.tick", [packed], orchestrator, algod_client, account)
    # AVM returns uint64 two's complement for negative int24
    assert r == to_int64(tick)


@pytest.mark.localnet
def test_slot0_tick_min_tick(helper47, orchestrator, algod_client, account):
    """MIN_TICK (-887272) round-trips correctly."""
    packed = make_slot0(sqrtPriceX96=1, tick=MIN_TICK)
    r = grouped_call(helper47, "Slot0Library.tick", [packed], orchestrator, algod_client, account)
    assert r == to_int64(MIN_TICK)


@pytest.mark.localnet
def test_slot0_tick_max_tick(helper47, orchestrator, algod_client, account):
    """MAX_TICK (887272) round-trips correctly."""
    packed = make_slot0(sqrtPriceX96=MAX_SQRT_PRICE, tick=MAX_TICK)
    r = grouped_call(helper47, "Slot0Library.tick", [packed], orchestrator, algod_client, account)
    assert r == MAX_TICK


# ===========================================================================
# Slot0Library.sqrtPriceX96 (Helper47)
# ===========================================================================

@pytest.mark.localnet
def test_slot0_sqrtPriceX96_zero(helper47, orchestrator, algod_client, account):
    """Slot0 with sqrtPriceX96 == 0 returns 0."""
    packed = make_slot0(sqrtPriceX96=0)
    r = grouped_call(helper47, "Slot0Library.sqrtPriceX96", [packed], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_slot0_sqrtPriceX96_1_1(helper47, orchestrator, algod_client, account):
    """SQRT_PRICE_1_1 round-trips correctly."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1)
    r = grouped_call(helper47, "Slot0Library.sqrtPriceX96", [packed], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1


@pytest.mark.localnet
def test_slot0_sqrtPriceX96_large(helper47, orchestrator, algod_client, account):
    """Large sqrtPriceX96 value fits in uint160 field."""
    price = (1 << 158)  # large but within uint160
    packed = make_slot0(sqrtPriceX96=price)
    r = grouped_call(helper47, "Slot0Library.sqrtPriceX96", [packed], orchestrator, algod_client, account)
    assert r == price


@pytest.mark.localnet
def test_slot0_sqrtPriceX96_unchanged_by_other_fields(helper47, orchestrator, algod_client, account):
    """sqrtPriceX96 is not polluted by tick/fee fields."""
    price = SQRT_PRICE_1_1
    packed = make_slot0(sqrtPriceX96=price, tick=-500, protocolFee=0x300, lpFee=0x1F4)
    r = grouped_call(helper47, "Slot0Library.sqrtPriceX96", [packed], orchestrator, algod_client, account)
    assert r == price


# ===========================================================================
# Slot0Library.setProtocolFee (Helper47)
# ===========================================================================

@pytest.mark.localnet
def test_slot0_setProtocolFee_zero(helper47, orchestrator, algod_client, account):
    """Setting protocolFee to 0 yields 0 when read back."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=100, protocolFee=0x300)
    r = grouped_call(helper47, "Slot0Library.setProtocolFee", [packed, 0], orchestrator, algod_client, account)
    result = bytes(r)
    # Extract protocolFee from bits 184-207 (bytes 6-8 in big-endian)
    val = int.from_bytes(result, 'big')
    protocol_fee_extracted = (val >> 184) & 0xFFFFFF
    assert protocol_fee_extracted == 0


@pytest.mark.localnet
def test_slot0_setProtocolFee_small_value(helper47, orchestrator, algod_client, account):
    """setProtocolFee stores the fee in the correct bit range."""
    fee = 0x0100  # 256
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=0)
    r = grouped_call(helper47, "Slot0Library.setProtocolFee", [packed, fee], orchestrator, algod_client, account)
    result = bytes(r)
    val = int.from_bytes(result, 'big')
    protocol_fee_extracted = (val >> 184) & 0xFFFFFF
    assert protocol_fee_extracted == fee


@pytest.mark.localnet
def test_slot0_setProtocolFee_preserves_other_fields(helper47, orchestrator, algod_client, account):
    """setProtocolFee does not disturb sqrtPriceX96, tick, or lpFee."""
    price = SQRT_PRICE_1_1
    tick = 200
    lp_fee = 3000
    packed = make_slot0(sqrtPriceX96=price, tick=tick, lpFee=lp_fee)
    new_proto_fee = 0x0200

    r = grouped_call(helper47, "Slot0Library.setProtocolFee", [packed, new_proto_fee], orchestrator, algod_client, account)
    result = bytes(r)
    val = int.from_bytes(result, 'big')

    sqrt_extracted  = val & ((1 << 160) - 1)
    tick_extracted  = (val >> 160) & 0xFFFFFF
    proto_extracted = (val >> 184) & 0xFFFFFF
    lp_extracted    = (val >> 208) & 0xFFFFFF

    assert sqrt_extracted  == price
    assert tick_extracted  == (tick & 0xFFFFFF)
    assert proto_extracted == new_proto_fee
    assert lp_extracted    == lp_fee


@pytest.mark.localnet
def test_slot0_setProtocolFee_max_uint24(helper47, orchestrator, algod_client, account):
    """Maximum uint24 protocol fee (0xFFFFFF) is stored without truncation."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1)
    fee = 0xFFFFFF
    r = grouped_call(helper47, "Slot0Library.setProtocolFee", [packed, fee], orchestrator, algod_client, account)
    result = bytes(r)
    val = int.from_bytes(result, 'big')
    proto_extracted = (val >> 184) & 0xFFFFFF
    assert proto_extracted == fee


# ===========================================================================
# Slot0Library.protocolFee (Helper49)
# ===========================================================================

@pytest.mark.localnet
def test_slot0_protocolFee_zero(helper49, orchestrator, algod_client, account):
    """Zero protocolFee is returned as 0."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, protocolFee=0)
    r = grouped_call(helper49, "Slot0Library.protocolFee", [packed], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_slot0_protocolFee_typical(helper49, orchestrator, algod_client, account):
    """Typical 2-byte protocol fee value round-trips correctly."""
    fee = 0x0300  # 768
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, protocolFee=fee)
    r = grouped_call(helper49, "Slot0Library.protocolFee", [packed], orchestrator, algod_client, account)
    assert r == fee


@pytest.mark.localnet
def test_slot0_protocolFee_max(helper49, orchestrator, algod_client, account):
    """Maximum uint24 protocolFee (0xFFFFFF) round-trips correctly."""
    fee = 0xFFFFFF
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, protocolFee=fee)
    r = grouped_call(helper49, "Slot0Library.protocolFee", [packed], orchestrator, algod_client, account)
    assert r == fee


@pytest.mark.localnet
def test_slot0_protocolFee_independent_of_other_fields(helper49, orchestrator, algod_client, account):
    """protocolFee extraction is not affected by sqrtPriceX96, tick, or lpFee."""
    fee = 0x1500
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=-100,
                        protocolFee=fee, lpFee=3000)
    r = grouped_call(helper49, "Slot0Library.protocolFee", [packed], orchestrator, algod_client, account)
    assert r == fee


# ===========================================================================
# Slot0Library.lpFee (Helper34)
# ===========================================================================

@pytest.mark.localnet
def test_slot0_lpFee_zero(helper34, orchestrator, algod_client, account):
    """Zero lpFee is returned as 0."""
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, lpFee=0)
    r = grouped_call(helper34, "Slot0Library.lpFee", [packed], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_slot0_lpFee_3000(helper34, orchestrator, algod_client, account):
    """Typical Uniswap 0.3% fee tier (3000) round-trips correctly."""
    fee = 3000
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, lpFee=fee)
    r = grouped_call(helper34, "Slot0Library.lpFee", [packed], orchestrator, algod_client, account)
    assert r == fee


@pytest.mark.localnet
def test_slot0_lpFee_10000(helper34, orchestrator, algod_client, account):
    """1% fee tier (10000) round-trips correctly."""
    fee = 10000
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, lpFee=fee)
    r = grouped_call(helper34, "Slot0Library.lpFee", [packed], orchestrator, algod_client, account)
    assert r == fee


@pytest.mark.localnet
def test_slot0_lpFee_max_uint24(helper34, orchestrator, algod_client, account):
    """Maximum uint24 lpFee (0xFFFFFF) round-trips correctly."""
    fee = 0xFFFFFF
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, lpFee=fee)
    r = grouped_call(helper34, "Slot0Library.lpFee", [packed], orchestrator, algod_client, account)
    assert r == fee


@pytest.mark.localnet
def test_slot0_lpFee_independent_of_other_fields(helper34, orchestrator, algod_client, account):
    """lpFee extraction is not polluted by sqrtPriceX96, tick, or protocolFee."""
    fee = 500
    packed = make_slot0(sqrtPriceX96=SQRT_PRICE_1_1, tick=-200,
                        protocolFee=0x0300, lpFee=fee)
    r = grouped_call(helper34, "Slot0Library.lpFee", [packed], orchestrator, algod_client, account)
    assert r == fee


# ===========================================================================
# equals (Helper34) — CurrencyLibrary.equals(address currency, address other)
# ===========================================================================

@pytest.mark.localnet
def test_equals_same_address(helper34, orchestrator, algod_client, account):
    """An address equals itself."""
    r = grouped_call(helper34, "equals", [ZERO_ADDR, ZERO_ADDR], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_equals_different_addresses(helper34, orchestrator, algod_client, account):
    """Two distinct addresses are not equal."""
    r = grouped_call(helper34, "equals", [ADDR_A, ADDR_B], orchestrator, algod_client, account)
    assert r is False


@pytest.mark.localnet
def test_equals_nonzero_address_with_itself(helper34, orchestrator, algod_client, account):
    """A non-zero address equals itself."""
    r = grouped_call(helper34, "equals", [ADDR_FF, ADDR_FF], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_equals_zero_vs_nonzero(helper34, orchestrator, algod_client, account):
    """Zero address does not equal a non-zero address."""
    r = grouped_call(helper34, "equals", [ZERO_ADDR, ADDR_A], orchestrator, algod_client, account)
    assert r is False


@pytest.mark.localnet
def test_equals_symmetry(helper34, orchestrator, algod_client, account):
    """equals(a, b) == equals(b, a) — symmetry property."""
    r1 = grouped_call(helper34, "equals", [ADDR_A, ADDR_B], orchestrator, algod_client, account)
    r2 = grouped_call(helper34, "equals", [ADDR_B, ADDR_A], orchestrator, algod_client, account)
    assert r1 == r2


# ===========================================================================
# greaterThanOrEqualTo (Helper38) — CurrencyLibrary.greaterThanOrEqualTo(address, address)
# ===========================================================================

@pytest.mark.localnet
def test_greaterThanOrEqualTo_equal_addresses(helper38, orchestrator, algod_client, account):
    """An address is >= itself."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ADDR_A, ADDR_A], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_greaterThanOrEqualTo_larger_is_gte(helper38, orchestrator, algod_client, account):
    """ADDR_B (0x02) >= ADDR_A (0x01)."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ADDR_B, ADDR_A], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_greaterThanOrEqualTo_smaller_is_not_gte(helper38, orchestrator, algod_client, account):
    """ADDR_A (0x01) is not >= ADDR_B (0x02)."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ADDR_A, ADDR_B], orchestrator, algod_client, account)
    assert r is False


@pytest.mark.localnet
def test_greaterThanOrEqualTo_zero_gte_zero(helper38, orchestrator, algod_client, account):
    """Zero address is >= zero address."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ZERO_ADDR, ZERO_ADDR], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_greaterThanOrEqualTo_max_gte_zero(helper38, orchestrator, algod_client, account):
    """Max address (all 0xff bytes) is >= zero address."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ADDR_FF, ZERO_ADDR], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_greaterThanOrEqualTo_zero_not_gte_max(helper38, orchestrator, algod_client, account):
    """Zero address is not >= max address."""
    r = grouped_call(helper38, "greaterThanOrEqualTo", [ZERO_ADDR, ADDR_FF], orchestrator, algod_client, account)
    assert r is False


# ===========================================================================
# neq (Helper38) — neq(uint256 a, uint256 b) -> bool
# ===========================================================================

@pytest.mark.localnet
def test_neq_equal_values(helper38, orchestrator, algod_client, account):
    """Equal values are not not-equal (i.e. neq returns False)."""
    val = 42
    r = grouped_call(helper38, "neq", [val, val], orchestrator, algod_client, account)
    assert r is False


@pytest.mark.localnet
def test_neq_different_values(helper38, orchestrator, algod_client, account):
    """Different values: neq returns True."""
    r = grouped_call(helper38, "neq", [1, 2], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_neq_zero_and_nonzero(helper38, orchestrator, algod_client, account):
    """0 and 1 are not equal."""
    r = grouped_call(helper38, "neq", [0, 1], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_neq_zero_equals_zero(helper38, orchestrator, algod_client, account):
    """0 and 0 are equal, so neq is False."""
    r = grouped_call(helper38, "neq", [0, 0], orchestrator, algod_client, account)
    assert r is False


@pytest.mark.localnet
def test_neq_large_uint256(helper38, orchestrator, algod_client, account):
    """Two large uint256 values that differ by 1 are not equal."""
    big = (1 << 255)
    r = grouped_call(helper38, "neq", [big, big + 1], orchestrator, algod_client, account)
    assert r is True


@pytest.mark.localnet
def test_neq_same_large_uint256(helper38, orchestrator, algod_client, account):
    """Same large uint256 value is equal, so neq is False."""
    big = (1 << 200) + 0xDEADBEEF
    r = grouped_call(helper38, "neq", [big, big], orchestrator, algod_client, account)
    assert r is False
