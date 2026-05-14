"""Tests for the strings category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_constant_string_literal(harness):
    """strings/contracts/constant_string_literal.sol"""
    app = harness.compile_and_deploy("strings/contracts/constant_string_literal.sol")
    # b() -> 0x6162636465666768696a6b6c6d6e6f7071000000000000000000000000000000
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 44048183304486788312148433451363384677561671644786151922963192794228216299520
    # x() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "x()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # getB() -> 0x6162636465666768696a6b6c6d6e6f7071000000000000000000000000000000
    r = harness.call(app, "getB()")
    assert as_int(r.abi_return) == 44048183304486788312148433451363384677561671644786151922963192794228216299520
    # getX() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "getX()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # getX2() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "getX2()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # unused() -> 2
    r = harness.call(app, "unused()")
    assert as_int(r.abi_return) == 2

def test_empty_storage_string(harness):
    """strings/contracts/empty_storage_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_storage_string.sol")
    # f() -> 0x20, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # g() -> 0x40, 0x60, 0, 0
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (64, 96, 0, 0)
    # h() -> 0x40, 0x60, 0, 0x1a, 38178759162904981154304545770567765692299154484752076569098748838215919075328
    r = harness.call(app, "h()")
    # TODO: verify structural decoding matches expected: 64, 96, 0, 26, 38178759162904981154304545770567765692299154484752076569098748838215919075328
    assert not r.reverted
    # i() -> 0x40, 0x80, 0x1a, 38178759162904981154304545770567765692299154484752076569098748838215919075328, 0
    r = harness.call(app, "i()")
    # TODO: verify structural decoding matches expected: 64, 128, 26, 38178759162904981154304545770567765692299154484752076569098748838215919075328, 0
    assert not r.reverted
    # j(string): 0x20, 0, "" -> 0x20, 0
    r = harness.call(app, "j(string)", '')
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # k() -> 0x20, 0
    r = harness.call(app, "k()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # l(string): 0x20, 0, "" -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "l(string)", '')
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # m() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "m()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # n() -> FAILURE, hex"d3f13430", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"0000000000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "n()", expect_revert=True)
    assert r.reverted
    # o() ->
    r = harness.call(app, "o()")
    # (void return — call succeeding is the assertion)
    # p() ->
    r = harness.call(app, "p()")
    # (void return — call succeeding is the assertion)
    # q() -> 0x20, 0
    r = harness.call(app, "q()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # r() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "r()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # s() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "s()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # set(string): 0x20, 0, "" ->
    r = harness.call(app, "set(string)", '')
    # (void return — call succeeding is the assertion)
    # get() -> 0x20, 0
    r = harness.call(app, "get()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)

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
