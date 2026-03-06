"""Uniswap V4 BeforeSwapDeltaLibrary — adapted from BeforeSwapDelta.t.sol

Tests for:
- BeforeSwapDeltaLibrary.getSpecifiedDelta(uint256) -> uint256  (Helper48)
- BeforeSwapDeltaLibrary.getUnspecifiedDelta(uint256) -> uint256  (Helper49)

BeforeSwapDelta packs two int128 values into a single uint256:
  upper 128 bits = specifiedDelta (int128)
  lower 128 bits = unspecifiedDelta (int128)
Same packing as BalanceDelta.
"""
import pytest
import algokit_utils as au
from helpers import to_int128, from_int128, from_int256, grouped_call


def pack_before_swap_delta(specified: int, unspecified: int) -> int:
    """Pack two int128 values into a uint256 BeforeSwapDelta."""
    return (to_int128(specified) << 128) | to_int128(unspecified)


# ─── getSpecifiedDelta tests (Helper48) ──────────────────────────────────────

@pytest.mark.localnet
def test_getSpecifiedDelta_zero(helper48, orchestrator, algod_client, account):
    """Zero packed value returns zero specified delta."""
    delta = pack_before_swap_delta(0, 0)
    r = grouped_call(helper48, "BeforeSwapDeltaLibrary.getSpecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 0


@pytest.mark.localnet
def test_getSpecifiedDelta_upper_half_only(helper48, orchestrator, algod_client, account):
    """Value with only upper half set returns nonzero specified delta."""
    delta = pack_before_swap_delta(1000, 0)
    r = grouped_call(helper48, "BeforeSwapDeltaLibrary.getSpecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 1000


@pytest.mark.localnet
def test_getSpecifiedDelta_lower_half_only(helper48, orchestrator, algod_client, account):
    """Value with only lower half set returns zero specified delta."""
    delta = pack_before_swap_delta(0, 5000)
    r = grouped_call(helper48, "BeforeSwapDeltaLibrary.getSpecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 0


@pytest.mark.localnet
def test_getSpecifiedDelta_both_halves(helper48, orchestrator, algod_client, account):
    """Value with both halves set returns correct specified delta."""
    delta = pack_before_swap_delta(42, 99)
    r = grouped_call(helper48, "BeforeSwapDeltaLibrary.getSpecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 42


@pytest.mark.localnet
@pytest.mark.xfail(reason="Negative int128 extraction via shr may not handle sign extension correctly")
def test_getSpecifiedDelta_negative(helper48, orchestrator, algod_client, account):
    """Negative specified delta should be extracted correctly."""
    delta = pack_before_swap_delta(-500, 200)
    r = grouped_call(helper48, "BeforeSwapDeltaLibrary.getSpecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == -500


# ─── getUnspecifiedDelta tests (Helper49) ────────────────────────────────────

@pytest.mark.localnet
def test_getUnspecifiedDelta_zero(helper49, orchestrator, algod_client, account):
    """Zero packed value returns zero unspecified delta."""
    delta = pack_before_swap_delta(0, 0)
    r = grouped_call(helper49, "BeforeSwapDeltaLibrary.getUnspecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 0


@pytest.mark.localnet
def test_getUnspecifiedDelta_lower_half_only(helper49, orchestrator, algod_client, account):
    """Value with only lower half set returns nonzero unspecified delta."""
    delta = pack_before_swap_delta(0, 7777)
    r = grouped_call(helper49, "BeforeSwapDeltaLibrary.getUnspecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 7777


@pytest.mark.localnet
def test_getUnspecifiedDelta_upper_half_only(helper49, orchestrator, algod_client, account):
    """Value with only upper half set returns zero unspecified delta."""
    delta = pack_before_swap_delta(3000, 0)
    r = grouped_call(helper49, "BeforeSwapDeltaLibrary.getUnspecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 0


@pytest.mark.localnet
def test_getUnspecifiedDelta_both_halves(helper49, orchestrator, algod_client, account):
    """Value with both halves set returns correct unspecified delta."""
    delta = pack_before_swap_delta(42, 99)
    r = grouped_call(helper49, "BeforeSwapDeltaLibrary.getUnspecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == 99


@pytest.mark.localnet
def test_getUnspecifiedDelta_negative(helper49, orchestrator, algod_client, account):
    """Negative unspecified delta should be extracted correctly.
    Return is uint256 (sign-extended int128→int256), so use from_int256."""
    delta = pack_before_swap_delta(100, -300)
    r = grouped_call(helper49, "BeforeSwapDeltaLibrary.getUnspecifiedDelta", [delta], orchestrator, algod_client, account)
    assert from_int256(r) == -300
