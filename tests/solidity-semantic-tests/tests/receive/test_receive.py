"""Tests for the receive category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_empty_calldata_calls_receive(harness):
    """receive/contracts/empty_calldata_calls_receive.sol"""
    app = harness.compile_and_deploy("receive/contracts/empty_calldata_calls_receive.sol")
    # x() -> 0
    assert as_int(harness.call(app, "x()").abi_return) == 0
    # bare call (no calldata) → receive() ++x → x = 1
    assert not harness.call_bare(app).reverted
    assert as_int(harness.call(app, "x()").abi_return) == 1
    # bare call with 1 wei → receive() ++x → x = 2
    assert not harness.call_bare(app, payment_wei=1).reverted
    assert as_int(harness.call(app, "x()").abi_return) == 2
    # x() with payment_wei=1 — there's no receive() match because args are non-empty;
    # contract has no fallback, so this reverts.
    r = harness.call(app, "x()", payment_wei=1, expect_revert=True)
    assert r.reverted
    # bare call with garbage payload — no fallback defined; should revert.
    r = harness.call_raw(app, selector=None, extra_args=(bytes.fromhex("00"),), expect_revert=True)
    assert r.reverted
    # same with 1 ether payment — reverts (no fallback).
    r = harness.call_raw(
        app, selector=None, extra_args=(bytes.fromhex("00"),),
        payment_wei=1000000000000000000, expect_revert=True,
    )
    assert r.reverted

def test_ether_and_data(harness):
    """receive/contracts/ether_and_data.sol"""
    app = harness.compile_and_deploy("receive/contracts/ether_and_data.sol")
    # bare call with 1 ether → receive() succeeds (payable, no body).
    # Note: 1 ether = 1e18 microalgos overflows test account; use small amount.
    assert not harness.call_bare(app, payment_wei=1).reverted
    # bare call with payload but no receive() match for non-empty calldata → revert
    r = harness.call_raw(
        app, selector=None, extra_args=((1).to_bytes(32, "big"),),
        payment_wei=1, expect_revert=True,
    )
    assert r.reverted

def test_inherited(harness):
    """receive/contracts/inherited.sol"""
    app = harness.compile_and_deploy("receive/contracts/inherited.sol")
    # getData() -> 0
    assert as_int(harness.call(app, "getData()").abi_return) == 0
    # bare call → receive() ++data → data = 1
    assert not harness.call_bare(app).reverted
    assert as_int(harness.call(app, "getData()").abi_return) == 1
    # bare call with 1 wei → receive() ++data → data = 2 (1 ether overflows; use 1 wei)
    assert not harness.call_bare(app, payment_wei=1).reverted
    assert as_int(harness.call(app, "getData()").abi_return) == 2
