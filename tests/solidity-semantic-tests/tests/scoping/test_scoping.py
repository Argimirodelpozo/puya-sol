"""Auto-generated tests for the scoping category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_c99_scoping_activation(harness):
    """scoping/contracts/c99_scoping_activation.sol"""
    app = harness.compile_and_deploy("scoping/contracts/c99_scoping_activation.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0
    # h() -> 3, 3, 4
    r = harness.call(app, "h()")
    assert tuple(r.abi_return) == (3, 3, 4)
    # i() -> 3, 3
    r = harness.call(app, "i()")
    assert tuple(r.abi_return) == (3, 3)
