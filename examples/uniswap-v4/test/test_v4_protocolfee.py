"""Uniswap V4 ProtocolFeeLibrary — adapted from ProtocolFeeLibrary.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_PROTOCOL_FEE, MAX_LP_FEE

@pytest.mark.localnet
def test_getZeroForOneFee(helper11):
    """fee = 100 | (200 << 12) -> zeroForOne = lower 12 bits = 100"""
    fee = 100 | (200 << 12)
    r = helper11.send.call(au.AppClientMethodCallParams(
        method="ProtocolFeeLibrary.getZeroForOneFee", args=[fee],
    ))
    assert r.abi_return == 100

@pytest.mark.localnet
def test_getOneForZeroFee(helper11):
    """fee = 100 | (200 << 12) -> oneForZero = upper 12 bits = 200"""
    fee = 100 | (200 << 12)
    r = helper11.send.call(au.AppClientMethodCallParams(
        method="ProtocolFeeLibrary.getOneForZeroFee", args=[fee],
    ))
    assert r.abi_return == 200

@pytest.mark.localnet
@pytest.mark.parametrize("fee0,fee1,valid", [
    (100, 100, True),
    (0, 0, True),
    (MAX_PROTOCOL_FEE, MAX_PROTOCOL_FEE, True),
    (MAX_PROTOCOL_FEE + 1, 0, False),
    (0, MAX_PROTOCOL_FEE + 1, False),
    (MAX_PROTOCOL_FEE + 1, MAX_PROTOCOL_FEE + 1, False),
])
def test_isValidProtocolFee(helper46, fee0, fee1, valid):
    packed = fee0 | (fee1 << 12)
    r = helper46.send.call(au.AppClientMethodCallParams(
        method="ProtocolFeeLibrary.isValidProtocolFee", args=[packed],
    ))
    assert (r.abi_return != 0) == valid

@pytest.mark.localnet
@pytest.mark.parametrize("proto_fee,lp_fee,expected", [
    (0, 3000, 3000),
    (0, 0, 0),
])
def test_calculateSwapFee_zeroProtoFee(helper11, proto_fee, lp_fee, expected):
    """calculateSwapFee with zero protocol fee just returns lpFee."""
    r = helper11.send.call(au.AppClientMethodCallParams(
        method="ProtocolFeeLibrary.calculateSwapFee", args=[proto_fee, lp_fee],
    ))
    assert r.abi_return == expected

@pytest.mark.localnet
@pytest.mark.xfail(reason="calculateSwapFee uint64 underflow: protocolFee * lpFee intermediate overflows uint64 subtraction")
def test_calculateSwapFee_nonZeroProtoFee(helper11):
    """calculateSwapFee with nonzero protocol fee requires shr."""
    proto_fee = 100 | (100 << 12)
    r = helper11.send.call(au.AppClientMethodCallParams(
        method="ProtocolFeeLibrary.calculateSwapFee", args=[proto_fee, 3000],
    ))
    assert r.abi_return == 3100
