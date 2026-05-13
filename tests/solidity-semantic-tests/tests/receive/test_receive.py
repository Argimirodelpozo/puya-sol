"""Auto-generated tests for the receive category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_empty_calldata_calls_receive(harness):
    """receive/empty_calldata_calls_receive.sol"""
    app = harness.compile_and_deploy("receive/empty_calldata_calls_receive.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # (), 1 wei
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
    # x(), 1 wei -> FAILURE
    r = harness.call(app, "x()", payment_wei=1, expect_revert=True)
    assert r.reverted
    # (): hex"00" -> FAILURE
    r = harness.call(app, "()", bytes.fromhex('00'), expect_revert=True)
    assert r.reverted
    # (), 1 ether: hex"00" -> FAILURE
    r = harness.call(app, "()", bytes.fromhex('00'), payment_wei=1000000000000000000, expect_revert=True)
    assert r.reverted

def test_ether_and_data(harness):
    """receive/ether_and_data.sol"""
    app = harness.compile_and_deploy("receive/ether_and_data.sol")
    # (), 1 ether
    pytest.xfail("fallback() dispatch not yet implemented")
    # (), 1 ether: 1 -> FAILURE
    r = harness.call(app, "()", 1, payment_wei=1000000000000000000, expect_revert=True)
    assert r.reverted

def test_inherited(harness):
    """receive/inherited.sol"""
    app = harness.compile_and_deploy("receive/inherited.sol")
    # getData() -> 0
    r = harness.call(app, "getData()")
    assert r.abi_return == 0
    # () ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # getData() -> 1
    r = harness.call(app, "getData()")
    assert r.abi_return == 1
    # (), 1 ether ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # getData() -> 2
    r = harness.call(app, "getData()")
    assert r.abi_return == 2
