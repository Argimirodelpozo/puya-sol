"""Uniswap V4 SafeCast — adapted from SafeCast.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT160, MAX_INT128, MIN_INT128, MAX_INT256
from helpers import to_int256

@pytest.mark.localnet
def test_toUint160_valid(helper25):
    r = helper25.send.call(au.AppClientMethodCallParams(method="SafeCast.toUint160", args=[MAX_UINT160]))
    assert r.abi_return == MAX_UINT160

@pytest.mark.localnet
def test_toUint160_overflow_reverts(helper25):
    with pytest.raises(Exception):
        helper25.send.call(au.AppClientMethodCallParams(method="SafeCast.toUint160", args=[MAX_UINT160 + 1]))

@pytest.mark.localnet
@pytest.mark.parametrize("val", [0, 1, MAX_UINT160 // 2, MAX_UINT160])
def test_fuzz_toUint160_valid(helper25, val):
    r = helper25.send.call(au.AppClientMethodCallParams(method="SafeCast.toUint160", args=[val]))
    assert r.abi_return == val

@pytest.mark.localnet
@pytest.mark.parametrize("val", [MAX_UINT160 + 1, MAX_UINT160 + 100])
def test_fuzz_toUint160_overflow_reverts(helper25, val):
    with pytest.raises(Exception):
        helper25.send.call(au.AppClientMethodCallParams(method="SafeCast.toUint160", args=[val]))

@pytest.mark.localnet
def test_toInt256_valid(helper30):
    r = helper30.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt256", args=[MAX_INT256]))
    assert r.abi_return == MAX_INT256

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: SafeCast overflow check not triggering revert for values > MAX_INT256")
def test_toInt256_overflow_reverts(helper30):
    with pytest.raises(Exception):
        helper30.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt256", args=[MAX_INT256 + 1]))

@pytest.mark.localnet
def test_toInt128_valid(helper34):
    r = helper34.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt128", args=[to_int256(MAX_INT128)]))
    assert r.abi_return == MAX_INT128

@pytest.mark.localnet
def test_toInt128_overflow_positive_reverts(helper34):
    with pytest.raises(Exception):
        helper34.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt128", args=[to_int256(MAX_INT128 + 1)]))

@pytest.mark.localnet
def test_toInt128_overflow_negative_reverts(helper34):
    with pytest.raises(Exception):
        helper34.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt128", args=[to_int256(MIN_INT128 - 1)]))

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: toInt128 with MIN_INT128 (two's complement edge case) produces unexpected result")
def test_toInt128_min_valid(helper34):
    r = helper34.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt128", args=[to_int256(MIN_INT128)]))
    # Two's complement: MIN_INT128 as uint256
    assert r.abi_return == to_int256(MIN_INT128) & ((1 << 128) - 1)

@pytest.mark.localnet
def test_toInt128_zero(helper34):
    r = helper34.send.call(au.AppClientMethodCallParams(method="SafeCast.toInt128", args=[0]))
    assert r.abi_return == 0
