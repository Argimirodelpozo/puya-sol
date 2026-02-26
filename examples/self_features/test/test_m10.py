"""
M10: Formatter library functions.
Exercises Formatter.numAsciiToUint, parseDatePart, substring, isLeapYear, toTimestamp, formatDate.
These are real Self.xyz library functions exercising: bytes element access, bytes1 type,
time unit literals (days), uint16 type, new bytes(N), string<->bytes conversion.
"""

import pytest
import calendar
from datetime import datetime, timezone

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def formatter_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "FormatterTest")


# --- numAsciiToUint tests ---

NUM_ASCII_VECTORS = [
    (48, 0),  # '0' = 48
    (49, 1),  # '1' = 49
    (57, 9),  # '9' = 57
    (50, 2),  # '2' = 50
]


@pytest.mark.localnet
@pytest.mark.parametrize("ascii_code,expected", NUM_ASCII_VECTORS)
def test_num_ascii_to_uint(
    formatter_client: au.AppClient, ascii_code: int, expected: int
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(method="testNumAsciiToUint", args=[ascii_code])
    )
    assert result.abi_return == expected


# --- parseDatePart tests ---

PARSE_DATE_PART_VECTORS = [
    ("23", 23),
    ("01", 1),
    ("12", 12),
    ("00", 0),
    ("99", 99),
    ("", 0),
]


@pytest.mark.localnet
@pytest.mark.parametrize("value,expected", PARSE_DATE_PART_VECTORS)
def test_parse_date_part(
    formatter_client: au.AppClient, value: str, expected: int
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(method="testParseDatePart", args=[value])
    )
    assert result.abi_return == expected


# --- substring tests ---

SUBSTRING_VECTORS = [
    ("Hello World", 0, 5, "Hello"),
    ("Hello World", 6, 11, "World"),
    ("abcdef", 2, 4, "cd"),
    ("230115", 0, 2, "23"),
    ("230115", 2, 4, "01"),
    ("230115", 4, 6, "15"),
]


@pytest.mark.localnet
@pytest.mark.parametrize("s,start,end,expected", SUBSTRING_VECTORS)
def test_substring(
    formatter_client: au.AppClient, s: str, start: int, end: int, expected: str
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(method="testSubstring", args=[s, start, end])
    )
    assert result.abi_return == expected


# --- isLeapYear tests ---

LEAP_YEAR_VECTORS = [
    (2000, True),   # divisible by 400 → leap
    (2024, True),   # divisible by 4, not 100 → leap
    (2023, False),  # not divisible by 4 → not leap
    (1972, True),   # divisible by 4 → leap
    (2100, False),  # divisible by 100, not 400 → not leap
    (1970, False),  # not divisible by 4 → not leap
]


@pytest.mark.localnet
@pytest.mark.parametrize("year,expected", LEAP_YEAR_VECTORS)
def test_is_leap_year(
    formatter_client: au.AppClient, year: int, expected: bool
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(method="testIsLeapYear", args=[year])
    )
    assert result.abi_return == expected


# --- toTimestamp tests ---


def python_to_timestamp(year: int, month: int, day: int) -> int:
    """Compute Unix timestamp matching Solidity's toTimestamp (midnight UTC)."""
    dt = datetime(year, month, day, tzinfo=timezone.utc)
    return int(dt.timestamp())


TO_TIMESTAMP_VECTORS = [
    (1970, 1, 1, 0),                          # epoch
    (2000, 1, 1, python_to_timestamp(2000, 1, 1)),  # Y2K
    (2023, 6, 15, python_to_timestamp(2023, 6, 15)),
    (2024, 2, 29, python_to_timestamp(2024, 2, 29)),  # leap year
    (1970, 12, 31, python_to_timestamp(1970, 12, 31)),
]


@pytest.mark.localnet
@pytest.mark.parametrize("year,month,day,expected", TO_TIMESTAMP_VECTORS)
def test_to_timestamp(
    formatter_client: au.AppClient, year: int, month: int, day: int, expected: int
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(
            method="testToTimestamp", args=[year, month, day]
        )
    )
    assert result.abi_return == expected


# --- formatDate tests ---

FORMAT_DATE_VECTORS = [
    ("230615", "15-06-23"),
    ("000101", "01-01-00"),
    ("991231", "31-12-99"),
    ("240229", "29-02-24"),
]


@pytest.mark.localnet
@pytest.mark.parametrize("date_input,expected", FORMAT_DATE_VECTORS)
def test_format_date(
    formatter_client: au.AppClient, date_input: str, expected: str
) -> None:
    result = formatter_client.send.call(
        au.AppClientMethodCallParams(method="testFormatDate", args=[date_input])
    )
    assert result.abi_return == expected
