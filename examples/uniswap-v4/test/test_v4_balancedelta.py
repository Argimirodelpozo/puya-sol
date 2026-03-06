"""Uniswap V4 BalanceDelta — adapted from BalanceDelta.t.sol"""
import pytest
import algokit_utils as au
from helpers import to_int128, from_int128, pack_balance_delta, grouped_call
from constants import MAX_INT128, MIN_INT128

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (0, 0),
    (100, 200),
    (-100, 200),
    (MAX_INT128, MIN_INT128),
    (-1, -1),
])
def test_toBalanceDelta(helper48, a0, a1, orchestrator, algod_client, account):
    r = grouped_call(helper48, "toBalanceDelta", [to_int128(a0), to_int128(a1)], orchestrator, algod_client, account)
    assert r == pack_balance_delta(a0, a1)

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (100, 200),
    (0, 0),
])
def test_amount0(helper48, a0, a1, orchestrator, algod_client, account):
    delta = pack_balance_delta(a0, a1)
    r = grouped_call(helper48, "BalanceDeltaLibrary.amount0", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == a0

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: amount0 with negative value — shr for int128 extraction compiled incorrectly")
@pytest.mark.parametrize("a0,a1", [
    (-50, 50),
])
def test_amount0_negative(helper48, a0, a1, orchestrator, algod_client, account):
    delta = pack_balance_delta(a0, a1)
    r = grouped_call(helper48, "BalanceDeltaLibrary.amount0", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == a0

@pytest.mark.localnet
@pytest.mark.parametrize("a0,a1", [
    (100, 200),
    (-50, 50),
    (0, 0),
])
def test_amount1(helper44, a0, a1, orchestrator, algod_client, account):
    delta = pack_balance_delta(a0, a1)
    r = grouped_call(helper44, "BalanceDeltaLibrary.amount1", [delta], orchestrator, algod_client, account)
    assert from_int128(r) == a1

@pytest.mark.localnet
def test_add(helper41, orchestrator, algod_client, account):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = grouped_call(helper41, "add", [a, b], orchestrator, algod_client, account)
    assert r == pack_balance_delta(15, 35)

@pytest.mark.localnet
def test_sub(helper39, orchestrator, algod_client, account):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = grouped_call(helper39, "sub", [a, b], orchestrator, algod_client, account)
    assert r == pack_balance_delta(5, 5)

@pytest.mark.localnet
def test_neq_true(helper38, orchestrator, algod_client, account):
    a = pack_balance_delta(10, 20)
    b = pack_balance_delta(5, 15)
    r = grouped_call(helper38, "neq", [a, b], orchestrator, algod_client, account)
    assert r == 1

@pytest.mark.localnet
def test_neq_false(helper38, orchestrator, algod_client, account):
    a = pack_balance_delta(10, 20)
    r = grouped_call(helper38, "neq", [a, a], orchestrator, algod_client, account)
    assert r == 0
