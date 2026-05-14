"""Tests for the experimental category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

pytestmark = pytest.mark.skip(reason="`pragma experimental solidity` — type-class / __builtin instantiation. Compiler-side: not supported in puya-sol.")


def test_stub(harness):
    """experimental/contracts/stub.sol"""

def test_type_class(harness):
    """experimental/contracts/type_class.sol"""
