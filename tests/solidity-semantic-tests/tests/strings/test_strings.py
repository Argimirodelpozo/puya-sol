"""Tests for the strings category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_constant_string_literal(harness):
    """strings/contracts/constant_string_literal.sol"""
    app = harness.compile_and_deploy("strings/contracts/constant_string_literal.sol")
    # bytes32 b = "abcdefghijklmnopq" — 17 chars padded with zeros.
    b_expected = b"abcdefghijklmnopq" + b"\x00" * 15
    x_expected = "abefghijklmnopqabcdefghijklmnopqabcdefghijklmnopqabca"
    assert bytes(harness.call(app, "b()").abi_return) == b_expected
    assert harness.call(app, "x()").abi_return == x_expected
    assert bytes(harness.call(app, "getB()").abi_return) == b_expected
    assert harness.call(app, "getX()").abi_return == x_expected
    assert harness.call(app, "getX2()").abi_return == x_expected
    assert as_int(harness.call(app, "unused()").abi_return) == 2

def test_empty_storage_string(harness):
    """strings/contracts/empty_storage_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_storage_string.sol")
    # f() returns empty string; g() returns (empty, empty).
    assert harness.call(app, "f()").abi_return == ""
    assert tuple(harness.call(app, "g()").abi_return) == ("", "")
    # h(), i() return non-trivial strings/tuples; just verify success.
    assert not harness.call(app, "h()").reverted
    assert not harness.call(app, "i()").reverted
    # j("") returns empty string echoed back.
    assert harness.call(app, "j(string)", "").abi_return == ""
    assert harness.call(app, "k()").abi_return == ""
    # l/m return `bytes` (abi.encode of empty string) — non-empty payload.
    assert not harness.call(app, "l(string)", "").reverted
    assert not harness.call(app, "m()").reverted
    # n() reverts (custom error with empty payload).
    assert harness.call(app, "n()", expect_revert=True).reverted
    # o(), p() are void-returning.
    assert not harness.call(app, "o()").reverted
    assert not harness.call(app, "p()").reverted
    # q/r/s return bytes blobs (raw byte lists); just verify success.
    assert bytes(harness.call(app, "q()").abi_return) == b""
    assert not harness.call(app, "r()").reverted
    assert not harness.call(app, "s()").reverted
    harness.call(app, "set(string)", "")
    assert harness.call(app, "get()").abi_return == ""

def test_empty_string(harness):
    """strings/contracts/empty_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_string.sol")
    # f() returns "".
    assert harness.call(app, "f()").abi_return == ""

def test_empty_string_input(harness):
    """strings/contracts/empty_string_input.sol

    The original isoltest fixture exercises calldata-tail-trimming
    behaviour (passing 0 extra bytes vs an empty `string` argument).
    With ARC4 dispatching there's no equivalent trim case — we just
    verify the canonical empty-string call shape works.
    """
    app = harness.compile_and_deploy("strings/contracts/empty_string_input.sol")
    assert harness.call(app, "f()").abi_return == ""
    assert harness.call(app, "g(string)", "").abi_return == ""
    # h(s, v) returns (s, v).
    r = harness.call(app, "h(string,uint256)", "", 0x888)
    assert r.abi_return[0] == ""
    assert as_int(r.abi_return[1]) == 0x888
    # i(msg1, v, msg2) returns (msg1, msg2, v) — note the deliberate reorder.
    r = harness.call(app, "i(string,uint256,string)", "", 0x888, "")
    assert (r.abi_return[0], r.abi_return[1], as_int(r.abi_return[2])) == ("", "", 0x888)
    # j(msg1, v) returns (msg1, "", v).
    r = harness.call(app, "j(string,uint256)", "", 0x888)
    assert (r.abi_return[0], r.abi_return[1], as_int(r.abi_return[2])) == ("", "", 0x888)

def test_return_string(harness):
    """strings/contracts/return_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/return_string.sol")
    # set(string): 0x20, 5, "Julia" ->
    r = harness.call(app, "set(string)", 'Julia')
    # (void return — call succeeding is the assertion)
    # get1() -> 0x20, 5, "Julia"
    r = harness.call(app, "get1()")
    assert r.abi_return == 'Julia'
    # get2() -> 0x20, 5, "Julia"
    r = harness.call(app, "get2()")
    assert r.abi_return == 'Julia'
    # s() -> 0x20, 5, "Julia"
    r = harness.call(app, "s()")
    assert r.abi_return == 'Julia'

def test_string_escapes(harness):
    """strings/contracts/string_escapes.sol"""
    app = harness.compile_and_deploy("strings/contracts/string_escapes.sol")
    # f() -> 0x090a0d27225c0000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 4088574885656074156409271147257688673164889634092489131729182745150422515712

def test_unicode_escapes(harness):
    """strings/contracts/unicode_escapes.sol"""
    app = harness.compile_and_deploy("strings/contracts/unicode_escapes.sol")
    # algosdk utf-8-decodes the returned `string`, so we compare against
    # the human-readable form (¢, €, etc.) rather than the raw bytes.
    assert harness.call(app, "oneByteUTF8()").abi_return == "aaa$aaa"
    assert harness.call(app, "twoBytesUTF8()").abi_return == "aaa¢aaa"
    assert harness.call(app, "threeBytesUTF8()").abi_return == "aaa€aaa"
    assert harness.call(app, "combined()").abi_return == "$¢€"


def test_unicode_string(harness):
    """strings/contracts/unicode_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/unicode_string.sol")
    expected = "😃, 😭, and 😈"
    assert harness.call(app, "f()").abi_return == expected
    assert harness.call(app, "g()").abi_return == expected
