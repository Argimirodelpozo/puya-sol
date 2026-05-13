"""Tests for the constantEvaluator category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_negative_fractional_mod(harness):
    """constantEvaluator/contracts/negative_fractional_mod.sol"""
    app = harness.compile_and_deploy("constantEvaluator/contracts/negative_fractional_mod.sol")
    # f() -> 11, 10
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (11, 10)

def test_rounding(harness):
    """constantEvaluator/contracts/rounding.sol"""
    app = harness.compile_and_deploy("constantEvaluator/contracts/rounding.sol")
    # f() -> 2, 2, 2, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 2, 2, 2)
