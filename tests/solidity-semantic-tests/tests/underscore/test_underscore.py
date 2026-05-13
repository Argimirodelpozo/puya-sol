"""Auto-generated tests for the underscore category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_as_function(harness):
    """underscore/as_function.sol"""
    app = harness.compile_and_deploy("underscore/as_function.sol")
    # _() -> 88
    r = harness.call(app, "_()")
    assert r.abi_return == 88
    # g() -> 88
    r = harness.call(app, "g()")
    assert r.abi_return == 88
    # h() -> 33
    r = harness.call(app, "h()")
    assert r.abi_return == 33
