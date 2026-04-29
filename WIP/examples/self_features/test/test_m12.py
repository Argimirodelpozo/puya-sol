"""
M12: CircuitAttributeHandlerV2 tests.
Exercises extractStringAttribute, getIssuingState, getDocumentNoOfac,
getOlderThan, compareOlderThan, and getFieldPositions for E_PASSPORT.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def handler_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "AttributeHandlerTest")


def make_passport_charcodes() -> bytes:
    """Build a 93-byte passport charcode array matching E_PASSPORT field positions.

    E_PASSPORT FieldPositions:
      issuingStateStart: 2, issuingStateEnd: 4   (3 bytes)
      nameStart: 5, nameEnd: 43                  (39 bytes)
      documentNumberStart: 44, documentNumberEnd: 52 (9 bytes)
      nationalityStart: 54, nationalityEnd: 56    (3 bytes)
      dateOfBirthStart: 57, dateOfBirthEnd: 62    (6 bytes)
      genderStart: 64, genderEnd: 64              (1 byte)
      expiryDateStart: 65, expiryDateEnd: 70      (6 bytes)
      olderThanStart: 88, olderThanEnd: 89        (2 bytes)
      ofacStart: 90, ofacEnd: 92                  (3 bytes)
    """
    data = bytearray(93)

    # Issuing state: "USA" at positions 2-4
    data[2] = ord("U")
    data[3] = ord("S")
    data[4] = ord("A")

    # Name: "JOHN<DOE" at positions 5-43 (pad rest with '<')
    name = b"JOHN<DOE"
    for i, c in enumerate(name):
        data[5 + i] = c
    for i in range(len(name), 39):
        data[5 + i] = ord("<")

    # OlderThan: "25" at positions 88-89 (ASCII digits)
    data[88] = ord("2")  # 50
    data[89] = ord("5")  # 53

    # OFAC: [1, 0, 1] at positions 90-92
    data[90] = 1
    data[91] = 0
    data[92] = 1

    return bytes(data)


# --- extractStringAttribute tests ---


@pytest.mark.localnet
def test_extract_string_simple(handler_client: au.AppClient) -> None:
    """Extract a 3-char string from positions 0-2."""
    charcodes = b"ABC" + b"\x00" * 10
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testExtractString",
            args=[charcodes, 0, 2],
        )
    )
    assert result.abi_return == "ABC"


@pytest.mark.localnet
def test_extract_string_middle(handler_client: au.AppClient) -> None:
    """Extract from the middle of a byte array."""
    charcodes = b"\x00\x00HELLO\x00\x00"
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testExtractString",
            args=[charcodes, 2, 6],
        )
    )
    assert result.abi_return == "HELLO"


@pytest.mark.localnet
def test_extract_string_single_char(handler_client: au.AppClient) -> None:
    """Extract single character (start == end)."""
    charcodes = b"XYZ"
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testExtractString",
            args=[charcodes, 1, 1],
        )
    )
    assert result.abi_return == "Y"


# --- getIssuingState tests ---


@pytest.mark.localnet
def test_get_issuing_state(handler_client: au.AppClient) -> None:
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testGetIssuingState",
            args=[charcodes],
        )
    )
    assert result.abi_return == "USA"


# --- getDocumentNoOfac tests ---


@pytest.mark.localnet
def test_get_document_no_ofac_true(handler_client: au.AppClient) -> None:
    """OFAC position 90 == 1 → True."""
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testGetDocumentNoOfac",
            args=[charcodes],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_get_document_no_ofac_false(handler_client: au.AppClient) -> None:
    """OFAC position 90 == 0 → False."""
    data = bytearray(make_passport_charcodes())
    data[90] = 0
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testGetDocumentNoOfac",
            args=[bytes(data)],
        )
    )
    assert result.abi_return is False


# --- getOlderThan tests ---


@pytest.mark.localnet
def test_get_older_than_25(handler_client: au.AppClient) -> None:
    """Passport with olderThan "25" → 2*10 + 5 = 25."""
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testGetOlderThan",
            args=[charcodes],
        )
    )
    # numAsciiToUint('2') = 50-48 = 2, numAsciiToUint('5') = 53-48 = 5
    # result = 2 * 10 + 5 = 25
    assert result.abi_return == 25


@pytest.mark.localnet
def test_get_older_than_18(handler_client: au.AppClient) -> None:
    """Passport with olderThan "18" → 1*10 + 8 = 18."""
    data = bytearray(make_passport_charcodes())
    data[88] = ord("1")
    data[89] = ord("8")
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testGetOlderThan",
            args=[bytes(data)],
        )
    )
    assert result.abi_return == 18


# --- compareOlderThan tests ---


@pytest.mark.localnet
def test_compare_older_than_pass(handler_client: au.AppClient) -> None:
    """25 >= 18 → True."""
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testCompareOlderThan",
            args=[charcodes, 18],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_compare_older_than_fail(handler_client: au.AppClient) -> None:
    """25 >= 30 → False."""
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testCompareOlderThan",
            args=[charcodes, 30],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_compare_older_than_equal(handler_client: au.AppClient) -> None:
    """25 >= 25 → True."""
    charcodes = make_passport_charcodes()
    result = handler_client.send.call(
        au.AppClientMethodCallParams(
            method="testCompareOlderThan",
            args=[charcodes, 25],
        )
    )
    assert result.abi_return is True


# --- getFieldPositions tests ---


@pytest.mark.localnet
def test_passport_name_start(handler_client: au.AppClient) -> None:
    """E_PASSPORT nameStart = 5."""
    result = handler_client.send.call(
        au.AppClientMethodCallParams(method="testPassportNameStart")
    )
    assert result.abi_return == 5


@pytest.mark.localnet
def test_passport_name_end(handler_client: au.AppClient) -> None:
    """E_PASSPORT nameEnd = 43."""
    result = handler_client.send.call(
        au.AppClientMethodCallParams(method="testPassportNameEnd")
    )
    assert result.abi_return == 43
