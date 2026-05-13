"""Tests for the accessor category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_accessor_for_const_state_variable(harness):
    """accessor/contracts/accessor_for_const_state_variable.sol"""
    app = harness.compile_and_deploy("accessor/contracts/accessor_for_const_state_variable.sol")
    # ticketPrice() -> 555
    r = harness.call(app, "ticketPrice()")
    assert as_int(r.abi_return) == 555

def test_accessor_for_state_variable(harness):
    """accessor/contracts/accessor_for_state_variable.sol"""
    app = harness.compile_and_deploy("accessor/contracts/accessor_for_state_variable.sol")
    # ticketPrice() -> 500
    r = harness.call(app, "ticketPrice()")
    assert as_int(r.abi_return) == 500
