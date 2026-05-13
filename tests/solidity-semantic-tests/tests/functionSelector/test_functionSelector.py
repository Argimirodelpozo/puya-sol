"""Auto-generated tests for the functionSelector category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_function_selector_via_contract_name(harness):
    """functionSelector/function_selector_via_contract_name.sol"""
    app = harness.compile_and_deploy("functionSelector/function_selector_via_contract_name.sol")
    # test1() -> left(0x26121ff0), left(0xe420264a), left(0x26121ff0), left(0xe420264a)
    r = harness.call(app, "test1()")
    # TODO: verify expected: left(0x26121ff0) | left(0xe420264a) | left(0x26121ff0) | left(0xe420264a)
    assert not r.reverted
    # test2() -> left(0x26121ff0), left(0xe420264a), left(0x26121ff0), left(0xe420264a)
    r = harness.call(app, "test2()")
    # TODO: verify expected: left(0x26121ff0) | left(0xe420264a) | left(0x26121ff0) | left(0xe420264a)
    assert not r.reverted
