"""Uniswap V4 BitMath — adapted from BitMath.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import MAX_UINT256


@pytest.mark.localnet
def test_mostSignificantBit_revertsWhenZero(helper44, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper44, "BitMath.mostSignificantBit", [0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mostSignificantBit_one(helper44, orchestrator, algod_client, account):
    r = grouped_call(helper44, "BitMath.mostSignificantBit", [1], orchestrator, algod_client, account)
    assert r == 0

@pytest.mark.localnet
def test_mostSignificantBit_two(helper44, orchestrator, algod_client, account):
    r = grouped_call(helper44, "BitMath.mostSignificantBit", [2], orchestrator, algod_client, account)
    assert r == 1

@pytest.mark.localnet
@pytest.mark.parametrize("power", [2, 4, 8, 16, 32, 64, 128, 255])
def test_mostSignificantBit_powersOfTwo(helper44, power, orchestrator, algod_client, account):
    r = grouped_call(helper44, "BitMath.mostSignificantBit", [1 << power], orchestrator, algod_client, account)
    assert r == power

@pytest.mark.localnet
def test_mostSignificantBit_maxUint256(helper44, orchestrator, algod_client, account):
    r = grouped_call(helper44, "BitMath.mostSignificantBit", [MAX_UINT256], orchestrator, algod_client, account)
    assert r == 255

@pytest.mark.localnet
@pytest.mark.parametrize("val,expected", [
    (256, 8),
    (1 << 128, 128),
    (42, 5),
    ((1 << 200) + 1, 200),
    (1 << 50, 50),
])
def test_fuzz_mostSignificantBit(helper44, val, expected, orchestrator, algod_client, account):
    r = grouped_call(helper44, "BitMath.mostSignificantBit", [val], orchestrator, algod_client, account)
    assert r == expected


@pytest.mark.localnet
def test_leastSignificantBit_revertsWhenZero(helper45, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper45, "BitMath.leastSignificantBit", [0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_leastSignificantBit_one(helper45, orchestrator, algod_client, account):
    r = grouped_call(helper45, "BitMath.leastSignificantBit", [1], orchestrator, algod_client, account)
    assert r == 0

@pytest.mark.localnet
def test_leastSignificantBit_two(helper45, orchestrator, algod_client, account):
    r = grouped_call(helper45, "BitMath.leastSignificantBit", [2], orchestrator, algod_client, account)
    assert r == 1

@pytest.mark.localnet
@pytest.mark.parametrize("power", [2, 4, 8, 16, 32, 64, 128, 255])
def test_leastSignificantBit_powersOfTwo(helper45, power, orchestrator, algod_client, account):
    r = grouped_call(helper45, "BitMath.leastSignificantBit", [1 << power], orchestrator, algod_client, account)
    assert r == power

@pytest.mark.localnet
def test_leastSignificantBit_maxUint256(helper45, orchestrator, algod_client, account):
    r = grouped_call(helper45, "BitMath.leastSignificantBit", [MAX_UINT256], orchestrator, algod_client, account)
    assert r == 0
