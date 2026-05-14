"""Tests for the shanghai category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_evmone_support(harness):
    """shanghai/contracts/evmone_support.sol"""
    pytest.fail("EVM-only: child contract ctor returns raw EVM bytecode via `return(0, 4)`. AVM has no bytecode access.")

def test_push0(harness):
    """shanghai/contracts/push0.sol"""
    app = harness.compile_and_deploy("shanghai/contracts/push0.sol")
    # zero() -> 0
    r = harness.call(app, "zero()")
    assert as_int(r.abi_return) == 0
