"""Tests for the metaTypes category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_name_other_contract(harness):
    """metaTypes/contracts/name_other_contract.sol"""
    app = harness.compile_and_deploy("metaTypes/contracts/name_other_contract.sol")
    # c() -> 0x20, 1, "C"
    r = harness.call(app, "c()")
    assert r.abi_return == 'C'
    # a() -> 0x20, 1, "A"
    r = harness.call(app, "a()")
    assert r.abi_return == 'A'
    # i() -> 0x20, 1, "I"
    r = harness.call(app, "i()")
    assert r.abi_return == 'I'
