"""Tests for the experimental category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

def test_stub(harness):  # currently fails
    """experimental/contracts/stub.sol"""
    app = harness.compile_and_deploy('experimental/contracts/stub.sol')

@pytest.mark.xfail(reason="uses Solidity `pragma experimental` features (type class) — experimental Solidity is not supported on AVM", strict=False)
def test_type_class(harness):  # currently fails
    """experimental/contracts/type_class.sol"""
    app = harness.compile_and_deploy('experimental/contracts/type_class.sol')
