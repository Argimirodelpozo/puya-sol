"""Auto-generated tests for the constantEvaluator category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_negative_fractional_mod(harness):
    """constantEvaluator/negative_fractional_mod.sol"""
    app = harness.compile_and_deploy("constantEvaluator/negative_fractional_mod.sol")
    # f() -> 11, 10
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (11, 10)

def test_rounding(harness):
    """constantEvaluator/rounding.sol"""
    app = harness.compile_and_deploy("constantEvaluator/rounding.sol")
    # f() -> 2, 2, 2, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (2, 2, 2, 2)
