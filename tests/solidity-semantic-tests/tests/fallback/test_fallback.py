"""Tests for the fallback category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


@pytest.mark.skip(reason="EVM-style replay-saved-calldata pattern: sender.fallback() saves msg.data, sender.forward() replays it via address(rec).call(savedData). On AVM the inner-call layout differs (ApplicationArgs slot-by-slot vs flat bytes), so the receiver's dispatcher can't decode the replayed payload.")
def test_call_forward_bytes(harness):
    """fallback/contracts/call_forward_bytes.sol"""

def test_falback_return(harness):
    """fallback/contracts/falback_return.sol"""
    app = harness.compile_and_deploy("fallback/contracts/falback_return.sol")
    # Bare () call → fallback() → x++. fallback caps x at 2.
    harness.call_bare(app)
    assert as_int(harness.call(app, "x()").abi_return) == 1
    harness.call_bare(app)
    assert as_int(harness.call(app, "x()").abi_return) == 2
    harness.call_bare(app)
    assert as_int(harness.call(app, "x()").abi_return) == 2
    harness.call_bare(app)
    assert as_int(harness.call(app, "x()").abi_return) == 2

def test_fallback_argument(harness):
    """fallback/contracts/fallback_argument.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_argument.sol")
    # f() does a self-call with "abc"; fallback sees 3 bytes, sets x = 3, returns "".
    r = harness.call(app, "f()")
    success, retval = r.abi_return[0], r.abi_return[1]
    assert bool(success) is True
    assert bytes(retval) == b""
    assert as_int(harness.call(app, "x()").abi_return) == 3

def test_fallback_argument_to_storage(harness):
    """fallback/contracts/fallback_argument_to_storage.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_argument_to_storage.sol")
    r = harness.call(app, "f()")
    assert bool(r.abi_return[0]) is True
    assert bytes(r.abi_return[1]) == b""
    # x() is a public bytes state var, decoded by algosdk as list[int].
    assert bytes(harness.call(app, "x()").abi_return) == b"abc"

def test_fallback_or_receive(harness):
    """fallback/contracts/fallback_or_receive.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_or_receive.sol")
    # f() -> (x, y) — bare call routes to receive() (++y), unknown-selector
    # routes to fallback() (++x).
    assert tuple(as_int(z) for z in harness.call(app, "f()").abi_return) == (0, 0)
    # bare call → receive()
    harness.call_bare(app)
    assert tuple(as_int(z) for z in harness.call(app, "f()").abi_return) == (0, 1)
    # bare call w/ ether payment → still routes to receive() on EVM, same here
    harness.call_bare(app, payment_wei=1)
    assert tuple(as_int(z) for z in harness.call(app, "f()").abi_return) == (0, 2)
    # unknown selector with 1 byte of data → fallback
    harness.call_raw(app, b"\x01\x02\x03\x04")
    assert tuple(as_int(z) for z in harness.call(app, "f()").abi_return) == (1, 2)
    # unknown selector + payment → fallback
    harness.call_raw(app, b"\x01\x02\x03\x04", payment_wei=1)
    assert tuple(as_int(z) for z in harness.call(app, "f()").abi_return) == (2, 2)

def test_fallback_override(harness):
    """fallback/contracts/fallback_override.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override.sol")
    # B's fallback overrides A's and returns "xyz".
    r = harness.call(app, "f()")
    assert bool(r.abi_return[0]) is True
    assert bytes(r.abi_return[1]) == b"xyz"

def test_fallback_override2(harness):
    """fallback/contracts/fallback_override2.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override2.sol")
    # B's no-arg fallback returns empty.
    r = harness.call(app, "f()")
    assert bool(r.abi_return[0]) is True
    assert bytes(r.abi_return[1]) == b""

def test_fallback_override_multi(harness):
    """fallback/contracts/fallback_override_multi.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override_multi.sol")
    r = harness.call(app, "f()")
    assert bool(r.abi_return[0]) is True
    assert bytes(r.abi_return[1]) == b""

def test_fallback_return_data(harness):
    """fallback/contracts/fallback_return_data.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_return_data.sol")
    # fallback echoes the input bytes — call sends "abc" → returns "abc".
    r = harness.call(app, "f()")
    assert bool(r.abi_return[0]) is True
    assert bytes(r.abi_return[1]) == b"abc"

def test_inherited(harness):
    """fallback/contracts/inherited.sol"""
    app = harness.compile_and_deploy("fallback/contracts/inherited.sol")
    assert as_int(harness.call(app, "getData()").abi_return) == 0
    # Unknown-selector call with 1 byte data (= 0x2a) routes to inherited fallback,
    # which sets data = 1.
    harness.call_raw(app, b"\x12\x34\x56\x78", extra_args=(bytes([42]),))
    assert as_int(harness.call(app, "getData()").abi_return) == 1

def test_short_data_calls_fallback(harness):
    """fallback/contracts/short_data_calls_fallback.sol"""
    app = harness.compile_and_deploy("fallback/contracts/short_data_calls_fallback.sol")
    # 3-byte selector "12b87d" — too short → fallback sets x=2.
    harness.call_raw(app, bytes.fromhex("12b87d"))
    assert as_int(harness.call(app, "x()").abi_return) == 2
    # Full 4-byte fow() selector "12b87db6" → fow() runs, sets x=3.
    harness.call_raw(app, bytes.fromhex("12b87db6"))
    assert as_int(harness.call(app, "x()").abi_return) == 3
    # 2-byte selector → fallback.
    harness.call_raw(app, bytes.fromhex("12b8"))
    assert as_int(harness.call(app, "x()").abi_return) == 2
    # 4-byte fow() selector again → fow() sets x=3.
    harness.call_raw(app, bytes.fromhex("12b87db6"))
    assert as_int(harness.call(app, "x()").abi_return) == 3
    # 1-byte selector → fallback.
    harness.call_raw(app, bytes.fromhex("12"))
    assert as_int(harness.call(app, "x()").abi_return) == 2
