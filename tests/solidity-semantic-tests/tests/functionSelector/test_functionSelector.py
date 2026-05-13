"""Tests for the functionSelector category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_function_selector_via_contract_name(harness):
    """functionSelector/contracts/function_selector_via_contract_name.sol"""
    app = harness.compile_and_deploy("functionSelector/contracts/function_selector_via_contract_name.sol")
    # test1() -> left(0x26121ff0), left(0xe420264a), left(0x26121ff0), left(0xe420264a)
    r = harness.call(app, "test1()")
    # TODO: verify expected: left(0x26121ff0) | left(0xe420264a) | left(0x26121ff0) | left(0xe420264a)
    assert not r.reverted
    # test2() -> left(0x26121ff0), left(0xe420264a), left(0x26121ff0), left(0xe420264a)
    r = harness.call(app, "test2()")
    # TODO: verify expected: left(0x26121ff0) | left(0xe420264a) | left(0x26121ff0) | left(0xe420264a)
    assert not r.reverted
