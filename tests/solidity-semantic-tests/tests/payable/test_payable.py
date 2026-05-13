"""Auto-generated tests for the payable category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_no_nonpayable_circumvention_by_modifier(harness):
    """payable/no_nonpayable_circumvention_by_modifier.sol"""
    app = harness.compile_and_deploy("payable/no_nonpayable_circumvention_by_modifier.sol")
    # f(), 27 wei -> FAILURE
    r = harness.call(app, "f()", payment_wei=27, expect_revert=True)
    assert r.reverted
