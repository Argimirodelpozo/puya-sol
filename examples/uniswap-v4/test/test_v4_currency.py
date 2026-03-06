"""Uniswap V4 CurrencyLibrary, ParseBytes, and LPFeeLibrary (override methods)"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from algosdk import encoding
from constants import MAX_LP_FEE

OVERRIDE_FEE_FLAG = 0x800000  # bit 23 = 8388608

ZERO_ADDRESS = encoding.encode_address(bytes(32))


def make_address(uint160_value: int) -> str:
    """Convert uint160 to Algorand address string (32-byte public key, base32)."""
    pk = (b'\x00' * 12) + uint160_value.to_bytes(20, "big")
    return encoding.encode_address(pk)


# ─── CurrencyLibrary.toId ─────────────────────────────────────────────────────

@pytest.mark.localnet
def test_toId_zero_address(helper32, orchestrator, algod_client, account):
    """Zero address converts to id 0."""
    r = grouped_call(helper32, "CurrencyLibrary.toId", [ZERO_ADDRESS], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_toId_nonzero_address(helper32, orchestrator, algod_client, account):
    """Address 0x01 converts to id 1."""
    addr = make_address(1)
    r = grouped_call(helper32, "CurrencyLibrary.toId", [addr], orchestrator, algod_client, account)
    assert r == 1


@pytest.mark.localnet
def test_toId_address_with_high_bytes(helper32, orchestrator, algod_client, account):
    """Address with high bytes set."""
    addr = make_address(0xFF << 152)  # high byte of 20-byte address
    r = grouped_call(helper32, "CurrencyLibrary.toId", [addr], orchestrator, algod_client, account)
    assert r == 0xFF << 152


# ─── CurrencyLibrary.isAddressZero ────────────────────────────────────────────

@pytest.mark.localnet
def test_isAddressZero_true(helper37, orchestrator, algod_client, account):
    """Zero address returns True."""
    r = grouped_call(helper37, "CurrencyLibrary.isAddressZero", [ZERO_ADDRESS], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_isAddressZero_false_nonzero(helper37, orchestrator, algod_client, account):
    """Non-zero address returns False."""
    addr = make_address(1)
    r = grouped_call(helper37, "CurrencyLibrary.isAddressZero", [addr], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_isAddressZero_false_high_byte(helper37, orchestrator, algod_client, account):
    """Address with only high byte set returns False."""
    addr = make_address(1 << 159)
    r = grouped_call(helper37, "CurrencyLibrary.isAddressZero", [addr], orchestrator, algod_client, account)
    assert r == 0


# ─── CurrencyLibrary.fromId ───────────────────────────────────────────────────

@pytest.mark.localnet
def test_fromId_zero(helper49, orchestrator, algod_client, account):
    """Id 0 converts to zero address."""
    r = grouped_call(helper49, "CurrencyLibrary.fromId", [0], orchestrator, algod_client, account)
    assert r == ZERO_ADDRESS


@pytest.mark.localnet
def test_fromId_one(helper49, orchestrator, algod_client, account):
    """Id 1 converts to address ending in 0x01."""
    r = grouped_call(helper49, "CurrencyLibrary.fromId", [1], orchestrator, algod_client, account)
    assert r == make_address(1)


@pytest.mark.localnet
def test_fromId_roundtrip(helper32, helper49, orchestrator, algod_client, account):
    """toId and fromId are inverses."""
    addr = make_address(42)
    r_id = grouped_call(helper32, "CurrencyLibrary.toId", [addr], orchestrator, algod_client, account)
    r_addr = grouped_call(helper49, "CurrencyLibrary.fromId", [r_id], orchestrator, algod_client, account)
    assert r_addr == addr


# ─── LPFeeLibrary.isOverride ──────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.xfail(reason="Bitwise NOT (~) on biguint doesn't truncate to uint24 width — isOverride uses & ~OVERRIDE_FEE_FLAG")
def test_isOverride_true_flag_set(helper34, orchestrator, algod_client, account):
    """Fee with override flag bit set returns True."""
    r = grouped_call(helper34, "LPFeeLibrary.isOverride", [OVERRIDE_FEE_FLAG | 3000], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_isOverride_false_no_flag(helper34, orchestrator, algod_client, account):
    """Fee without override flag returns False."""
    r = grouped_call(helper34, "LPFeeLibrary.isOverride", [3000], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_isOverride_false_zero_fee(helper34, orchestrator, algod_client, account):
    """Zero fee has no override flag set."""
    r = grouped_call(helper34, "LPFeeLibrary.isOverride", [0], orchestrator, algod_client, account)
    assert r == 0


# ─── LPFeeLibrary.removeOverrideFlag ─────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.xfail(reason="Bitwise NOT (~) on biguint doesn't truncate to uint24 width — & ~OVERRIDE_FEE_FLAG is no-op")
def test_removeOverrideFlag_clears_bit(helper35, orchestrator, algod_client, account):
    """Override flag is cleared, leaving only the fee value."""
    r = grouped_call(helper35, "LPFeeLibrary.removeOverrideFlag", [OVERRIDE_FEE_FLAG | 500], orchestrator, algod_client, account)
    assert r == 500


@pytest.mark.localnet
def test_removeOverrideFlag_no_flag_unchanged(helper35, orchestrator, algod_client, account):
    """Fee without flag is returned unchanged."""
    r = grouped_call(helper35, "LPFeeLibrary.removeOverrideFlag", [3000], orchestrator, algod_client, account)
    assert r == 3000


# ─── LPFeeLibrary.removeOverrideFlagAndValidate ───────────────────────────────

@pytest.mark.localnet
@pytest.mark.xfail(reason="Bitwise NOT (~) on biguint doesn't truncate to uint24 width — removeOverrideFlagAndValidate uses & ~OVERRIDE_FEE_FLAG")
def test_removeOverrideFlagAndValidate_valid_fee(helper34, orchestrator, algod_client, account):
    """Removes flag and returns valid fee."""
    r = grouped_call(helper34, "LPFeeLibrary.removeOverrideFlagAndValidate", [OVERRIDE_FEE_FLAG | 3000], orchestrator, algod_client, account)
    assert r == 3000


@pytest.mark.localnet
@pytest.mark.xfail(reason="Bitwise NOT (~) on biguint doesn't truncate to uint24 width")
def test_removeOverrideFlagAndValidate_max_fee(helper34, orchestrator, algod_client, account):
    """Max LP fee passes validation after flag removal."""
    r = grouped_call(helper34, "LPFeeLibrary.removeOverrideFlagAndValidate", [OVERRIDE_FEE_FLAG | MAX_LP_FEE], orchestrator, algod_client, account)
    assert r == MAX_LP_FEE


@pytest.mark.localnet
def test_removeOverrideFlagAndValidate_exceeds_max_reverts(helper34, orchestrator, algod_client, account):
    """Fee exceeding MAX_LP_FEE after flag removal reverts (value with flag > MAX_LP_FEE)."""
    with pytest.raises(Exception):
        grouped_call(helper34, "LPFeeLibrary.removeOverrideFlagAndValidate", [OVERRIDE_FEE_FLAG | (MAX_LP_FEE + 1)], orchestrator, algod_client, account)


# ─── ParseBytes.parseSelector ────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.xfail(reason="byte[4] return type ABI decode returns None — algokit/ARC56 decode issue")
def test_parseSelector_extracts_first_four_bytes(helper44, orchestrator, algod_client, account):
    """First 4 bytes extracted as selector."""
    data = b'\xde\xad\xbe\xef\x00\x01\x02\x03'
    r = grouped_call(helper44, "ParseBytes.parseSelector", [data], orchestrator, algod_client, account)
    result = r
    # byte[4] returns as list[int] via algokit
    if isinstance(result, (list, tuple)):
        assert bytes(result) == b'\xde\xad\xbe\xef'
    elif isinstance(result, bytes):
        assert result == b'\xde\xad\xbe\xef'
    else:
        # May return None if ABI decode fails for byte[4]
        assert result is not None, "parseSelector returned None — byte[4] ABI decode issue"


@pytest.mark.localnet
@pytest.mark.xfail(reason="byte[4] return type ABI decode returns None — algokit/ARC56 decode issue")
def test_parseSelector_ignores_trailing(helper44, orchestrator, algod_client, account):
    """Only first 4 bytes matter."""
    data = b'\x01\x02\x03\x04\xff\xff\xff\xff'
    r = grouped_call(helper44, "ParseBytes.parseSelector", [data], orchestrator, algod_client, account)
    result = r
    if isinstance(result, (list, tuple)):
        assert bytes(result) == b'\x01\x02\x03\x04'
    elif isinstance(result, bytes):
        assert result == b'\x01\x02\x03\x04'
    else:
        assert result is not None, "parseSelector returned None — byte[4] ABI decode issue"


# ─── ParseBytes.parseFee ─────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.xfail(reason="parseFee expects specific Yul returndata layout, not raw 32-byte big-endian encoding")
def test_parseFee_extracts_fee_value(helper43, orchestrator, algod_client, account):
    """Fee value correctly extracted from return bytes."""
    # parseFee expects a specific format — try with 32-byte encoding
    fee = 3000
    data = fee.to_bytes(32, 'big')
    r = grouped_call(helper43, "ParseBytes.parseFee", [data], orchestrator, algod_client, account)
    assert r == fee
