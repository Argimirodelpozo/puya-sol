"""Tests for the payable category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_no_nonpayable_circumvention_by_modifier(harness):
    """payable/contracts/no_nonpayable_circumvention_by_modifier.sol"""
    app = harness.compile_and_deploy("payable/contracts/no_nonpayable_circumvention_by_modifier.sol")
    # f(), 27 wei -> FAILURE
    r = harness.call(app, "f()", payment_wei=27, expect_revert=True)
    assert r.reverted
