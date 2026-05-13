"""Tests for the experimental category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_stub(harness):
    """experimental/contracts/stub.sol"""
    app = harness.compile_and_deploy("experimental/contracts/stub.sol", via_yul_behavior=True)
    # (): 0 -> 0
    pytest.xfail("fallback() dispatch not yet implemented")
    # (): 1 -> 544
    pytest.xfail("fallback() dispatch not yet implemented")

def test_type_class(harness):
    """experimental/contracts/type_class.sol"""
    app = harness.compile_and_deploy("experimental/contracts/type_class.sol", via_yul_behavior=True)
    # () -> 1, 0
    pytest.xfail("fallback() dispatch not yet implemented")
