"""Tests for the statements category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_do_while_loop_continue(harness):
    """statements/contracts/do_while_loop_continue.sol"""
    app = harness.compile_and_deploy("statements/contracts/do_while_loop_continue.sol")
    # f() -> 42
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 42

def test_empty_for_loop(harness):
    """statements/contracts/empty_for_loop.sol"""
    app = harness.compile_and_deploy("statements/contracts/empty_for_loop.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 10
