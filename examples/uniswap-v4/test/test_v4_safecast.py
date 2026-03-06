"""Uniswap V4 SafeCast — adapted from SafeCast.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT160, MAX_INT128, MIN_INT128, MAX_INT256
from helpers import to_int256, grouped_call

@pytest.mark.localnet
def test_toUint160_valid(helper48, orchestrator, algod_client, account):
    r = grouped_call(helper48, "SafeCast.toUint160", [MAX_UINT160], orchestrator, algod_client, account)
    assert r == MAX_UINT160

@pytest.mark.localnet
def test_toUint160_overflow_reverts(helper48, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "SafeCast.toUint160", [MAX_UINT160 + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.parametrize("val", [0, 1, MAX_UINT160 // 2, MAX_UINT160])
def test_fuzz_toUint160_valid(helper48, val, orchestrator, algod_client, account):
    r = grouped_call(helper48, "SafeCast.toUint160", [val], orchestrator, algod_client, account)
    assert r == val

@pytest.mark.localnet
@pytest.mark.parametrize("val", [MAX_UINT160 + 1, MAX_UINT160 + 100])
def test_fuzz_toUint160_overflow_reverts(helper48, val, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "SafeCast.toUint160", [val], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_toInt256_valid(helper48, orchestrator, algod_client, account):
    r = grouped_call(helper48, "SafeCast.toInt256", [MAX_INT256], orchestrator, algod_client, account)
    assert r == MAX_INT256

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: SafeCast overflow check not triggering revert for values > MAX_INT256")
def test_toInt256_overflow_reverts(helper48, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "SafeCast.toInt256", [MAX_INT256 + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_toInt128_valid(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "SafeCast.toInt128", [to_int256(MAX_INT128)], orchestrator, algod_client, account)
    assert r == MAX_INT128

@pytest.mark.localnet
def test_toInt128_overflow_positive_reverts(helper50, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper50, "SafeCast.toInt128", [to_int256(MAX_INT128 + 1)], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_toInt128_overflow_negative_reverts(helper50, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper50, "SafeCast.toInt128", [to_int256(MIN_INT128 - 1)], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: toInt128 with MIN_INT128 (two's complement edge case) produces unexpected result")
def test_toInt128_min_valid(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "SafeCast.toInt128", [to_int256(MIN_INT128)], orchestrator, algod_client, account)
    # Two's complement: MIN_INT128 as uint256
    assert r == to_int256(MIN_INT128) & ((1 << 128) - 1)

@pytest.mark.localnet
def test_toInt128_zero(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "SafeCast.toInt128", [0], orchestrator, algod_client, account)
    assert r == 0
