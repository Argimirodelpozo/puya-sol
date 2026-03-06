"""Uniswap V4 TickBitmap.nextInitializedTickWithinOneWord — extended tests.

Tests for:
- TickBitmap.nextInitializedTickWithinOneWord__chunk_0(byte[], uint64, uint64, bool)
    -> (uint64, bool, uint64) on Helper40

Args: bitmap (byte[] state), tick, tickSpacing, lte (search direction)
Returns: (next, initialized, compressed) — the next tick, whether it's initialized,
         and the compressed tick value.

With an empty bitmap (no initialized ticks), the search should find no
initialized tick within the word.
"""
import pytest
import algokit_utils as au
from helpers import to_int64, grouped_call


# ─── nextInitializedTickWithinOneWord chunk_0 (Helper40) ─────────────────────

@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_lte_tick0(helper40, orchestrator, algod_client, account):
    """Empty bitmap, tick=0, spacing=1, lte=True: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 0, 1, True], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    # initialized flag should be False (0) since bitmap is empty
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_gte_tick0(helper40, orchestrator, algod_client, account):
    """Empty bitmap, tick=0, spacing=1, lte=False: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 0, 1, False], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    # initialized flag should be False (0) since bitmap is empty
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_positive_tick(helper40, orchestrator, algod_client, account):
    """Empty bitmap, tick=100, spacing=1, lte=True: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 100, 1, True], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_spacing10(helper40, orchestrator, algod_client, account):
    """Empty bitmap, tick=0, spacing=10, lte=True: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 0, 10, True], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_negative_tick(helper40, orchestrator, algod_client, account):
    """Empty bitmap, negative tick, spacing=1, lte=True: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', to_int64(-100), 1, True], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_empty_bitmap_large_spacing_gte(helper40, orchestrator, algod_client, account):
    """Empty bitmap, tick=0, spacing=60, lte=False: no initialized tick found."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 0, 60, False], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    assert result[1] == 0, f"Expected not initialized, got {result[1]}"


@pytest.mark.localnet
def test_nextInitializedTick_returns_valid_next_tick(helper40, orchestrator, algod_client, account):
    """Verify the returned next tick value is a valid uint64."""
    r = grouped_call(helper40, "TickBitmap.nextInitializedTickWithinOneWord__chunk_0", [b'', 255, 1, True], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 3
    # next tick value should be a valid integer
    assert isinstance(result[0], int)
    assert result[0] >= 0
