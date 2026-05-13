"""Auto-generated tests for the saltedCreate category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_prediction_example(harness):
    """saltedCreate/prediction_example.sol"""
    app = harness.compile_and_deploy("saltedCreate/prediction_example.sol")
    # createDSalted(bytes32,uint256): 42, 64 ->
    r = harness.call(app, "createDSalted(bytes32,uint256)", 42, 64)
    # (void return — call succeeding is the assertion)

def test_salted_create(harness):
    """saltedCreate/salted_create.sol"""
    app = harness.compile_and_deploy("saltedCreate/salted_create.sol")
    # different_salt() -> true
    r = harness.call(app, "different_salt()")
    assert r.abi_return is True
    # same_salt() -> true
    r = harness.call(app, "same_salt()")
    assert r.abi_return is True

def test_salted_create_with_value(harness):
    """saltedCreate/salted_create_with_value.sol"""
    app = harness.compile_and_deploy("saltedCreate/salted_create_with_value.sol")
    # f(), 10 ether -> 3007, 3008, 3009
    r = harness.call(app, "f()", payment_wei=10000000000000000000)
    assert tuple(r.abi_return) == (3007, 3008, 3009)
