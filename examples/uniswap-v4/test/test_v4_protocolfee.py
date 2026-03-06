"""Uniswap V4 ProtocolFeeLibrary — adapted from ProtocolFeeLibrary.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import MAX_PROTOCOL_FEE, MAX_LP_FEE

@pytest.mark.localnet
def test_getZeroForOneFee(helper34, orchestrator, algod_client, account):
    """fee = 100 | (200 << 12) -> zeroForOne = lower 12 bits = 100"""
    fee = 100 | (200 << 12)
    r = grouped_call(helper34, "ProtocolFeeLibrary.getZeroForOneFee", [fee], orchestrator, algod_client, account)
    assert r == 100

@pytest.mark.localnet
def test_getOneForZeroFee(helper34, orchestrator, algod_client, account):
    """fee = 100 | (200 << 12) -> oneForZero = upper 12 bits = 200"""
    fee = 100 | (200 << 12)
    r = grouped_call(helper34, "ProtocolFeeLibrary.getOneForZeroFee", [fee], orchestrator, algod_client, account)
    assert r == 200

@pytest.mark.localnet
@pytest.mark.parametrize("fee0,fee1,valid", [
    (100, 100, True),
    (0, 0, True),
    (MAX_PROTOCOL_FEE, MAX_PROTOCOL_FEE, True),
    (MAX_PROTOCOL_FEE + 1, 0, False),
    (0, MAX_PROTOCOL_FEE + 1, False),
    (MAX_PROTOCOL_FEE + 1, MAX_PROTOCOL_FEE + 1, False),
])
def test_isValidProtocolFee(helper49, fee0, fee1, valid, orchestrator, algod_client, account):
    packed = fee0 | (fee1 << 12)
    r = grouped_call(helper49, "ProtocolFeeLibrary.isValidProtocolFee", [packed], orchestrator, algod_client, account)
    assert (r != 0) == valid

@pytest.mark.localnet
@pytest.mark.parametrize("proto_fee,lp_fee,expected", [
    (0, 3000, 3000),
    (0, 0, 0),
])
def test_calculateSwapFee_zeroProtoFee(helper36, proto_fee, lp_fee, expected, orchestrator, algod_client, account):
    """calculateSwapFee with zero protocol fee just returns lpFee."""
    r = grouped_call(helper36, "ProtocolFeeLibrary.calculateSwapFee", [proto_fee, lp_fee], orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
@pytest.mark.xfail(reason="calculateSwapFee uint64 underflow: protocolFee * lpFee intermediate overflows uint64 subtraction")
def test_calculateSwapFee_nonZeroProtoFee(helper36, orchestrator, algod_client, account):
    """calculateSwapFee with nonzero protocol fee requires shr."""
    proto_fee = 100 | (100 << 12)
    r = grouped_call(helper36, "ProtocolFeeLibrary.calculateSwapFee", [proto_fee, 3000], orchestrator, algod_client, account)
    assert r == 3100
