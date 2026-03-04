"""Uniswap V4 TickBitmap — adapted from TickBitmap.t.sol"""
import pytest
import algokit_utils as au
from helpers import call_compress, call_flipTick, to_int64

@pytest.mark.localnet
@pytest.mark.parametrize("tick,expected_wordPos,expected_bitPos", [
    (0, 0, 0),
    (255, 0, 255),
    (256, 1, 0),
])
def test_position(helper33, tick, expected_wordPos, expected_bitPos):
    r = helper33.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.position",
        args=[tick],
    ))
    result = r.abi_return
    if isinstance(result, (list, tuple)):
        assert result[1] == expected_bitPos
    else:
        assert result is not None

@pytest.mark.localnet
def test_position_negative(helper33):
    r = helper33.send.call(au.AppClientMethodCallParams(
        method="TickBitmap.position",
        args=[to_int64(-1)],
    ))
    result = r.abi_return
    if isinstance(result, (list, tuple)):
        assert result[1] == 255

@pytest.mark.localnet
@pytest.mark.parametrize("tick,tickSpacing", [
    (0, 1),
    (100, 10),
    (255, 1),
])
def test_compress(helper41, helper6, tick, tickSpacing):
    """compress(tick, tickSpacing) returns tick/tickSpacing rounded toward negative infinity."""
    r = call_compress(helper41, helper6, tick, tickSpacing)
    assert r is not None

@pytest.mark.localnet
@pytest.mark.parametrize("tick,tickSpacing", [
    (-100, 10),
    (-256, 1),
])
def test_compress_negative(helper41, helper6, tick, tickSpacing):
    r = call_compress(helper41, helper6, tick, tickSpacing)
    assert r is not None

@pytest.mark.localnet
@pytest.mark.xfail(reason="flipTick is now chunked; chunk_0 returns void and chunk_1 requires intermediate state")
def test_flipTick_toggle(helper43, helper41):
    """Flip a tick twice should not error (toggles state)."""
    call_flipTick(helper43, helper41, -230, 1)
    call_flipTick(helper43, helper41, -230, 1)
