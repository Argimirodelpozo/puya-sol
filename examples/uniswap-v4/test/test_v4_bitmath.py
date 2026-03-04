"""Uniswap V4 BitMath — adapted from BitMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT256


def _call_msb(helper29, helper37, val):
    """Call chunked mostSignificantBit: chunk_0 on Helper29, chunk_1 on Helper37."""
    r0 = helper29.send.call(au.AppClientMethodCallParams(method="BitMath.mostSignificantBit__chunk_0", args=[val]))
    # chunk_1 takes (x: uint256, r: uint64) — original value + intermediate result
    r1 = helper37.send.call(au.AppClientMethodCallParams(method="BitMath.mostSignificantBit__chunk_1", args=[val, r0.abi_return]))
    return r1


@pytest.mark.localnet
def test_mostSignificantBit_revertsWhenZero(helper29, helper37):
    with pytest.raises(Exception):
        _call_msb(helper29, helper37, 0)

@pytest.mark.localnet
def test_mostSignificantBit_one(helper29, helper37):
    r = _call_msb(helper29, helper37, 1)
    assert r.abi_return == 0

@pytest.mark.localnet
def test_mostSignificantBit_two(helper29, helper37):
    r = _call_msb(helper29, helper37, 2)
    assert r.abi_return == 1

@pytest.mark.localnet
@pytest.mark.parametrize("power", [2, 4, 8, 16, 32, 64, 128, 255])
def test_mostSignificantBit_powersOfTwo(helper29, helper37, power):
    r = _call_msb(helper29, helper37, 1 << power)
    assert r.abi_return == power

@pytest.mark.localnet
def test_mostSignificantBit_maxUint256(helper29, helper37):
    r = _call_msb(helper29, helper37, MAX_UINT256)
    assert r.abi_return == 255

@pytest.mark.localnet
@pytest.mark.parametrize("val,expected", [
    (256, 8),
    (1 << 128, 128),
    (42, 5),
    ((1 << 200) + 1, 200),
    (1 << 50, 50),
])
def test_fuzz_mostSignificantBit(helper29, helper37, val, expected):
    r = _call_msb(helper29, helper37, val)
    assert r.abi_return == expected

def _call_lsb(helper36, helper34, val):
    """Call chunked leastSignificantBit: chunk_0 on Helper36, chunk_1 on Helper34."""
    r0 = helper36.send.call(au.AppClientMethodCallParams(method="BitMath.leastSignificantBit__chunk_0", args=[val]))
    r1 = helper34.send.call(au.AppClientMethodCallParams(method="BitMath.leastSignificantBit__chunk_1", args=[val, r0.abi_return]))
    return r1


@pytest.mark.localnet
def test_leastSignificantBit_revertsWhenZero(helper36, helper34):
    with pytest.raises(Exception):
        _call_lsb(helper36, helper34, 0)

@pytest.mark.localnet
def test_leastSignificantBit_one(helper36, helper34):
    r = _call_lsb(helper36, helper34, 1)
    assert r.abi_return == 0

@pytest.mark.localnet
def test_leastSignificantBit_two(helper36, helper34):
    r = _call_lsb(helper36, helper34, 2)
    assert r.abi_return == 1

@pytest.mark.localnet
@pytest.mark.parametrize("power", [2, 4, 8, 16, 32, 64, 128, 255])
def test_leastSignificantBit_powersOfTwo(helper36, helper34, power):
    r = _call_lsb(helper36, helper34, 1 << power)
    assert r.abi_return == power

@pytest.mark.localnet
def test_leastSignificantBit_maxUint256(helper36, helper34):
    r = _call_lsb(helper36, helper34, MAX_UINT256)
    assert r.abi_return == 0
