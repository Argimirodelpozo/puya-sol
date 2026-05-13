"""Tests for the scoping category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_c99_scoping_activation(harness):
    """scoping/contracts/c99_scoping_activation.sol"""
    app = harness.compile_and_deploy("scoping/contracts/c99_scoping_activation.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # h() -> 3, 3, 4
    r = harness.call(app, "h()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 3, 4)
    # i() -> 3, 3
    r = harness.call(app, "i()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 3)
