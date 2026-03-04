"""Uniswap V4 BalanceDelta — adapted from BalanceDelta.t.sol"""
import pytest
import algokit_utils as au
from helpers import to_int128, from_int128, pack_balance_delta
from constants import MAX_INT128, MIN_INT128

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (0, 0),
    (100, 200),
    (-100, 200),
    (MAX_INT128, MIN_INT128),
    (-1, -1),
])
def test_toBalanceDelta(helper28, a0, a1):
    r = helper28.send.call(au.AppClientMethodCallParams(
        method="toBalanceDelta",
        args=[to_int128(a0), to_int128(a1)],
    ))
    assert r.abi_return == pack_balance_delta(a0, a1)

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (100, 200),
    (0, 0),
])
def test_amount0(helper46, a0, a1):
    delta = pack_balance_delta(a0, a1)
    r = helper46.send.call(au.AppClientMethodCallParams(
        method="BalanceDeltaLibrary.amount0",
        args=[delta],
    ))
    assert from_int128(r.abi_return) == a0

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: amount0 with negative value — shr for int128 extraction compiled incorrectly")
@pytest.mark.parametrize("a0,a1", [
    (-50, 50),
])
def test_amount0_negative(helper46, a0, a1):
    delta = pack_balance_delta(a0, a1)
    r = helper46.send.call(au.AppClientMethodCallParams(
        method="BalanceDeltaLibrary.amount0",
        args=[delta],
    ))
    assert from_int128(r.abi_return) == a0

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (100, 200),
    (-50, 50),
    (0, 0),
])
def test_amount1(helper26, a0, a1):
    delta = pack_balance_delta(a0, a1)
    r = helper26.send.call(au.AppClientMethodCallParams(
        method="BalanceDeltaLibrary.amount1",
        args=[delta],
    ))
    assert from_int128(r.abi_return) == a1

@pytest.mark.localnet
def test_add(helper38):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = helper38.send.call(au.AppClientMethodCallParams(
        method="add",
        args=[a, b],
    ))
    assert r.abi_return == pack_balance_delta(15, 35)

@pytest.mark.localnet
def test_sub(helper35):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="sub",
        args=[a, b],
    ))
    assert r.abi_return == pack_balance_delta(5, 5)

@pytest.mark.localnet
def test_neq_true(helper16):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = helper16.send.call(au.AppClientMethodCallParams(
        method="neq",
        args=[a, b],
    ))
    assert r.abi_return == 1

@pytest.mark.localnet
def test_neq_false(helper16):
    a = pack_balance_delta(10, 20)
    r = helper16.send.call(au.AppClientMethodCallParams(
        method="neq",
        args=[a, a],
    ))
    assert r.abi_return == 0
