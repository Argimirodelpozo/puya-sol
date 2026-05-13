"""Tests for the underscore category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_as_function(harness):
    """underscore/contracts/as_function.sol"""
    app = harness.compile_and_deploy("underscore/contracts/as_function.sol")
    # _() -> 88
    r = harness.call(app, "_()")
    assert as_int(r.abi_return) == 88
    # g() -> 88
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 88
    # h() -> 33
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 33
