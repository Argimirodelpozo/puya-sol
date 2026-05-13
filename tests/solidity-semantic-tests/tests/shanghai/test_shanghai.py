"""Auto-generated tests for the shanghai category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_evmone_support(harness):
    """shanghai/contracts/evmone_support.sol"""
    app = harness.compile_and_deploy("shanghai/contracts/evmone_support.sol")
    # bytecode() -> 0x20, 4, 0x60205ff300000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "bytecode()")
    assert tuple(r.abi_return) == (32, 4, 43479234787236466803551061304173321300670960283493682185538338749387338940416)
    # isPush0Supported() -> true
    r = harness.call(app, "isPush0Supported()")
    assert r.abi_return is True

def test_push0(harness):
    """shanghai/contracts/push0.sol"""
    app = harness.compile_and_deploy("shanghai/contracts/push0.sol")
    # zero() -> 0
    r = harness.call(app, "zero()")
    assert r.abi_return == 0
