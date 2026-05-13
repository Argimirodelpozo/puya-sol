"""Auto-generated tests for the exponentiation category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_literal_base(harness):
    """exponentiation/contracts/literal_base.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/literal_base.sol")
    # f(uint256): 0 -> 1, 1
    r = harness.call(app, "f(uint256)", 0)
    assert tuple(r.abi_return) == (1, 1)
    # f(uint256): 1 -> 2, -2
    r = harness.call(app, "f(uint256)", 1)
    assert tuple(r.abi_return) == (2, -2)
    # f(uint256): 2 -> 4, 4
    r = harness.call(app, "f(uint256)", 2)
    assert tuple(r.abi_return) == (4, 4)
    # f(uint256): 13 -> 0x2000, -8192
    r = harness.call(app, "f(uint256)", 13)
    assert tuple(r.abi_return) == (8192, -8192)
    # f(uint256): 113 -> 0x020000000000000000000000000000, -10384593717069655257060992658440192
    r = harness.call(app, "f(uint256)", 113)
    assert tuple(r.abi_return) == (10384593717069655257060992658440192, -10384593717069655257060992658440192)
    # f(uint256): 114 -> 0x040000000000000000000000000000, 20769187434139310514121985316880384
    r = harness.call(app, "f(uint256)", 114)
    assert tuple(r.abi_return) == (20769187434139310514121985316880384, 20769187434139310514121985316880384)
    # f(uint256): 1113 -> 0x00, 0
    r = harness.call(app, "f(uint256)", 1113)
    assert tuple(r.abi_return) == (0, 0)
    # f(uint256): 1114 -> 0x00, 0
    r = harness.call(app, "f(uint256)", 1114)
    assert tuple(r.abi_return) == (0, 0)

def test_signed_base(harness):
    """exponentiation/contracts/signed_base.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/signed_base.sol")
    # f() -> 9, -27
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (9, -27)

def test_small_exp(harness):
    """exponentiation/contracts/small_exp.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/small_exp.sol")
    # f() -> 4
    r = harness.call(app, "f()")
    assert r.abi_return == 4
