"""
M11: CircuitConstantsV2 and SelfUtils tests.
Exercises struct creation with named fields, bytes32 constants, bytes32 equality,
struct field access, string-to-bytes conversion, left shift, bitwise OR.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def circuit_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "CircuitConstantsTest")


# --- getDiscloseIndices tests ---

# Expected values from CircuitConstantsV2.sol
# E_PASSPORT: nullifierIndex=7, scopeIndex=19
# EU_ID_CARD: nullifierIndex=8
# AADHAAR: nullifierIndex=0
# KYC: nullifierIndex=14


@pytest.mark.localnet
def test_passport_nullifier_index(circuit_client: au.AppClient) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testPassportNullifierIndex")
    )
    assert result.abi_return == 7


@pytest.mark.localnet
def test_passport_scope_index(circuit_client: au.AppClient) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testPassportScopeIndex")
    )
    assert result.abi_return == 19


@pytest.mark.localnet
def test_eu_id_nullifier_index(circuit_client: au.AppClient) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testEuIdNullifierIndex")
    )
    assert result.abi_return == 8


@pytest.mark.localnet
def test_aadhaar_nullifier_index(circuit_client: au.AppClient) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testAadhaarNullifierIndex")
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_kyc_nullifier_index(circuit_client: au.AppClient) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testKycNullifierIndex")
    )
    assert result.abi_return == 14


# --- stringToBigInt tests ---


def python_string_to_bigint(s: str) -> int:
    """Match SelfUtils.stringToBigInt — big-endian ASCII encoding."""
    result = 0
    for c in s.encode("ascii"):
        result = (result << 8) | c
    return result


STRING_TO_BIGINT_VECTORS = [
    ("A", python_string_to_bigint("A")),  # 65
    ("AB", python_string_to_bigint("AB")),  # 16706
    ("Hello", python_string_to_bigint("Hello")),
    ("", 0),
    ("0", python_string_to_bigint("0")),  # 48
]


@pytest.mark.localnet
@pytest.mark.parametrize("value,expected", STRING_TO_BIGINT_VECTORS)
def test_string_to_big_int(
    circuit_client: au.AppClient, value: str, expected: int
) -> None:
    result = circuit_client.send.call(
        au.AppClientMethodCallParams(method="testStringToBigInt", args=[value])
    )
    assert result.abi_return == expected
