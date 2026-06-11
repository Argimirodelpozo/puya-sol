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


def test_nonpayable_rejects_value(harness):
    """payable/contracts/nonpayable_rejects_value.sol

    CUSTOM regression guard (NOT vendored). EVM semantics: calling a
    non-payable function with value attached must revert (the group's payment
    rolls back atomically); payable accepts it; non-payable without value
    works. Pins the router's msg.value guard.
    """
    app = harness.compile_and_deploy("payable/contracts/nonpayable_rejects_value.sol")
    assert harness.call(app, "paid(uint256)", 7, payment_wei=1000).reverted is False
    assert harness.call(app, "plain(uint256)", 9, payment_wei=1000, expect_revert=True).reverted is True
    assert harness.call(app, "plain(uint256)", 5).reverted is False
