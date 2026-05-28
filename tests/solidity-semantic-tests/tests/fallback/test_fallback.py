"""Tests for the fallback category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_call_forward_bytes(harness):
    """fallback/contracts/call_forward_bytes.sol

    The sender contract stores msg.data (ApplicationArgs[0]) in savedData when
    an unknown selector hits its fallback(), then forward() replays that raw
    calldata to the inner receiver contract via address(rec).call(savedData).

    On AVM, msg.data == ApplicationArgs[0], so we must pack the full EVM
    calldata (selector + abi-encoded args) into a single ApplicationArgs[0]
    blob using call_raw.

    receiver.recv(uint256) ARC4 selector: sha512_256("recv(uint256)void")[:4]
    = 0x3bd31555.  The uint256 arg is ABI-encoded as 32 bytes big-endian.
    receiver.fallback() is triggered when savedData holds no matching selector
    (e.g. after clear() which stores empty bytes).
    """
    import hashlib
    recv_sel = hashlib.new('sha512_256', b'recv(uint256)void').digest()[:4]

    app = harness.compile_and_deploy('fallback/contracts/call_forward_bytes.sol',
                                     postinit_inner_txns=2)

    # Send recv(uint256):7 as raw calldata to sender's fallback — stores it in savedData.
    # selector(4) + uint256(7) as 32-byte big-endian = 36 bytes in ApplicationArgs[0].
    raw_call = recv_sel + (7).to_bytes(32, 'big')
    harness.call_raw(app, raw_call, extra_fee=2000)

    r = harness.call(app, 'val()')
    assert as_int(r.abi_return) == 0

    # forward() replays savedData to receiver → receiver.recv(7) → received = 7+1 = 8
    r = harness.call(app, 'forward()', extra_fee=2000)
    assert r.abi_return is True
    r = harness.call(app, 'val()')
    assert as_int(r.abi_return) == 8

    r = harness.call(app, 'clear()')
    assert r.abi_return is True
    r = harness.call(app, 'val()')
    assert as_int(r.abi_return) == 8

    # forward() with savedData cleared (empty) → receiver's fallback() → received = 0x80
    r = harness.call(app, 'forward()', extra_fee=2000)
    assert r.abi_return is True
    r = harness.call(app, 'val()')
    assert as_int(r.abi_return) == 0x80

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
