"""Auto-generated tests for the integer category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_basic(harness):
    """integer/contracts/basic.sol"""
    app = harness.compile_and_deploy("integer/contracts/basic.sol")
    # basic() -> true
    r = harness.call(app, "basic()")
    assert r.abi_return is True

def test_int(harness):
    """integer/contracts/int.sol"""
    app = harness.compile_and_deploy("integer/contracts/int.sol")
    # intMinA() -> true
    r = harness.call(app, "intMinA()")
    assert r.abi_return is True
    # intMinB() -> true
    r = harness.call(app, "intMinB()")
    assert r.abi_return is True
    # intMinC() -> true
    r = harness.call(app, "intMinC()")
    assert r.abi_return is True
    # intMinD() -> true
    r = harness.call(app, "intMinD()")
    assert r.abi_return is True
    # intMaxA() -> true
    r = harness.call(app, "intMaxA()")
    assert r.abi_return is True
    # intMaxB() -> true
    r = harness.call(app, "intMaxB()")
    assert r.abi_return is True
    # intMaxC() -> true
    r = harness.call(app, "intMaxC()")
    assert r.abi_return is True
    # intMaxD() -> true
    r = harness.call(app, "intMaxD()")
    assert r.abi_return is True

def test_many_local_variables(harness):
    """integer/contracts/many_local_variables.sol"""
    app = harness.compile_and_deploy("integer/contracts/many_local_variables.sol")
    # run(uint256,uint256,uint256): 0x1000, 0x10000, 0x100000 -> 0x121121
    r = harness.call(app, "run(uint256,uint256,uint256)", 4096, 65536, 1048576)
    assert r.abi_return == 1184033

def test_small_signed_types(harness):
    """integer/contracts/small_signed_types.sol"""
    app = harness.compile_and_deploy("integer/contracts/small_signed_types.sol")
    # run() -> 200
    r = harness.call(app, "run()")
    assert r.abi_return == 200

def test_uint(harness):
    """integer/contracts/uint.sol"""
    app = harness.compile_and_deploy("integer/contracts/uint.sol")
    # uintMinA() -> true
    r = harness.call(app, "uintMinA()")
    assert r.abi_return is True
    # uintMinB() -> true
    r = harness.call(app, "uintMinB()")
    assert r.abi_return is True
    # uintMinC() -> true
    r = harness.call(app, "uintMinC()")
    assert r.abi_return is True
    # uintMinD() -> true
    r = harness.call(app, "uintMinD()")
    assert r.abi_return is True
    # uintMaxA() -> true
    r = harness.call(app, "uintMaxA()")
    assert r.abi_return is True
    # uintMaxB() -> true
    r = harness.call(app, "uintMaxB()")
    assert r.abi_return is True
    # uintMaxC() -> true
    r = harness.call(app, "uintMaxC()")
    assert r.abi_return is True
    # uintMaxD() -> true
    r = harness.call(app, "uintMaxD()")
    assert r.abi_return is True
