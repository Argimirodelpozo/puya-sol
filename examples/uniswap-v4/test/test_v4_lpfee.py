"""Uniswap V4 LPFeeLibrary — adapted from LPFeeLibrary.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import MAX_LP_FEE, DYNAMIC_FEE_FLAG

@pytest.mark.localnet
def test_isDynamicFee_returnsTrue(helper37, orchestrator, algod_client, account):
    r = grouped_call(helper37, "LPFeeLibrary.isDynamicFee", [DYNAMIC_FEE_FLAG], orchestrator, algod_client, account)
    assert r != 0

@pytest.mark.localnet
@pytest.mark.parametrize("fee", [0, 500, 3000, MAX_LP_FEE])
def test_isDynamicFee_returnsFalse(helper37, fee, orchestrator, algod_client, account):
    r = grouped_call(helper37, "LPFeeLibrary.isDynamicFee", [fee], orchestrator, algod_client, account)
    assert r == 0

@pytest.mark.localnet
def test_validate_doesNotRevertWithNoFee(helper50, orchestrator, algod_client, account):
    grouped_call(helper50, "LPFeeLibrary.validate", [0], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_validate_doesNotRevert(helper50, orchestrator, algod_client, account):
    grouped_call(helper50, "LPFeeLibrary.validate", [3000], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_validate_doesNotRevertWithMaxFee(helper50, orchestrator, algod_client, account):
    grouped_call(helper50, "LPFeeLibrary.validate", [MAX_LP_FEE], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_validate_revertsWithLPFeeTooLarge(helper50, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper50, "LPFeeLibrary.validate", [MAX_LP_FEE + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_getInitialLPFee_forStaticFeeIsCorrect(helper40, orchestrator, algod_client, account):
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee", [3000], orchestrator, algod_client, account)
    assert r == 3000

@pytest.mark.localnet
def test_getInitialLPFee_revertsWithLPFeeTooLarge(helper40, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper40, "LPFeeLibrary.getInitialLPFee", [MAX_LP_FEE + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_getInitialLPFee_forDynamicFeeIsZero(helper40, orchestrator, algod_client, account):
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee", [DYNAMIC_FEE_FLAG], orchestrator, algod_client, account)
    assert r == 0

@pytest.mark.localnet
def test_getInitialLPFee_revertsWithNonExactDynamicFee(helper40, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper40, "LPFeeLibrary.getInitialLPFee", [DYNAMIC_FEE_FLAG + 1], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.parametrize("fee,expected", [
    (0, 0),
    (500, 500),
    (MAX_LP_FEE, MAX_LP_FEE),
    (DYNAMIC_FEE_FLAG, 0),
])
def test_fuzz_getInitialLPFee(helper40, fee, expected, orchestrator, algod_client, account):
    r = grouped_call(helper40, "LPFeeLibrary.getInitialLPFee", [fee], orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
def test_isValid_true(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "LPFeeLibrary.isValid", [3000], orchestrator, algod_client, account)
    assert r != 0

@pytest.mark.localnet
def test_isValid_false(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "LPFeeLibrary.isValid", [MAX_LP_FEE + 1], orchestrator, algod_client, account)
    assert r == 0
