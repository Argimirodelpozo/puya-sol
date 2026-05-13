"""Auto-generated tests for the statements category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_do_while_loop_continue(harness):
    """statements/contracts/do_while_loop_continue.sol"""
    app = harness.compile_and_deploy("statements/contracts/do_while_loop_continue.sol")
    # f() -> 42
    r = harness.call(app, "f()")
    assert r.abi_return == 42

def test_empty_for_loop(harness):
    """statements/contracts/empty_for_loop.sol"""
    app = harness.compile_and_deploy("statements/contracts/empty_for_loop.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert r.abi_return == 10
