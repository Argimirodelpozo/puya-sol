"""Auto-generated tests for the accessor category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_accessor_for_const_state_variable(harness):
    """accessor/accessor_for_const_state_variable.sol"""
    app = harness.compile_and_deploy("accessor/accessor_for_const_state_variable.sol")
    # ticketPrice() -> 555
    r = harness.call(app, "ticketPrice()")
    assert r.abi_return == 555

def test_accessor_for_state_variable(harness):
    """accessor/accessor_for_state_variable.sol"""
    app = harness.compile_and_deploy("accessor/accessor_for_state_variable.sol")
    # ticketPrice() -> 500
    r = harness.call(app, "ticketPrice()")
    assert r.abi_return == 500
