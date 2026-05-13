"""Auto-generated tests for the strings category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_constant_string_literal(harness):
    """strings/contracts/constant_string_literal.sol"""
    app = harness.compile_and_deploy("strings/contracts/constant_string_literal.sol")
    # b() -> 0x6162636465666768696a6b6c6d6e6f7071000000000000000000000000000000
    r = harness.call(app, "b()")
    assert r.abi_return == 44048183304486788312148433451363384677561671644786151922963192794228216299520
    # x() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # getB() -> 0x6162636465666768696a6b6c6d6e6f7071000000000000000000000000000000
    r = harness.call(app, "getB()")
    assert r.abi_return == 44048183304486788312148433451363384677561671644786151922963192794228216299520
    # getX() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "getX()")
    assert tuple(r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # getX2() -> 0x20, 0x35, 0x616265666768696a6b6c6d6e6f70716162636465666768696a6b6c6d6e6f7071, 44048183304486788312148433451363384677562177293131179093971701692629931524096
    r = harness.call(app, "getX2()")
    assert tuple(r.abi_return) == (32, 53, 44048197162110825789672587045853586576998041571578496415725902894201041809521, 44048183304486788312148433451363384677562177293131179093971701692629931524096)
    # unused() -> 2
    r = harness.call(app, "unused()")
    assert r.abi_return == 2

def test_empty_storage_string(harness):
    """strings/contracts/empty_storage_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_storage_string.sol")
    # f() -> 0x20, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 0)
    # g() -> 0x40, 0x60, 0, 0
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (64, 96, 0, 0)
    # h() -> 0x40, 0x60, 0, 0x1a, 38178759162904981154304545770567765692299154484752076569098748838215919075328
    r = harness.call(app, "h()")
    # TODO: verify structural decoding matches expected: 64, 96, 0, 26, 38178759162904981154304545770567765692299154484752076569098748838215919075328
    assert not r.reverted
    # i() -> 0x40, 0x80, 0x1a, 38178759162904981154304545770567765692299154484752076569098748838215919075328, 0
    r = harness.call(app, "i()")
    # TODO: verify structural decoding matches expected: 64, 128, 26, 38178759162904981154304545770567765692299154484752076569098748838215919075328, 0
    assert not r.reverted
    # j(string): 0x20, 0, "" -> 0x20, 0
    r = harness.call(app, "j(string)", 32, 0, bytes.fromhex(''))
    assert tuple(r.abi_return) == (32, 0)
    # k() -> 0x20, 0
    r = harness.call(app, "k()")
    assert tuple(r.abi_return) == (32, 0)
    # l(string): 0x20, 0, "" -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "l(string)", 32, 0, bytes.fromhex(''))
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # m() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "m()")
    assert tuple(r.abi_return) == (32, 64, 32, 0)
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
    assert tuple(r.abi_return) == (32, 0)
    # r() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "r()")
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # s() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # set(string): 0x20, 0, "" ->
    r = harness.call(app, "set(string)", 32, 0, bytes.fromhex(''))
    # (void return — call succeeding is the assertion)
    # get() -> 0x20, 0
    r = harness.call(app, "get()")
    assert tuple(r.abi_return) == (32, 0)

def test_empty_string(harness):
    """strings/contracts/empty_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_string.sol")
    # f() -> 0x20, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 0)

def test_empty_string_input(harness):
    """strings/contracts/empty_string_input.sol"""
    app = harness.compile_and_deploy("strings/contracts/empty_string_input.sol")
    # f() -> 0x20, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 0)
    # g(string): 0x20, 0, "" -> 0x20, 0
    r = harness.call(app, "g(string)", 32, 0, bytes.fromhex(''))
    assert tuple(r.abi_return) == (32, 0)
    # g(string): 0x20, 0 -> 0x20, 0
    r = harness.call(app, "g(string)", 32, 0)
    assert tuple(r.abi_return) == (32, 0)
    # h(string,uint256): 0x40, 0x888, 0, "" -> 0x40, 0x0888, 0
    r = harness.call(app, "h(string,uint256)", 64, 2184, 0, bytes.fromhex(''))
    assert tuple(r.abi_return) == (64, 2184, 0)
    # h(string,uint256): 0x40, 0x888, 0 -> 0x40, 0x0888, 0
    r = harness.call(app, "h(string,uint256)", 64, 2184, 0)
    assert tuple(r.abi_return) == (64, 2184, 0)
    # i(string,uint256,string): 0x60, 0x888, 0x60, 0, "" -> 0x60, 0x80, 0x0888, 0, 0
    r = harness.call(app, "i(string,uint256,string)", 96, 2184, 96, 0, bytes.fromhex(''))
    # TODO: verify structural decoding matches expected: 96, 128, 2184, 0, 0
    assert not r.reverted
    # i(string,uint256,string): 0x60, 0x888, 0x60, 0 -> 0x60, 0x80, 0x0888, 0, 0
    r = harness.call(app, "i(string,uint256,string)", 96, 2184, 96, 0)
    # TODO: verify structural decoding matches expected: 96, 128, 2184, 0, 0
    assert not r.reverted
    # j(string,uint256): 0x40, 0x888, 0, "" -> 0x60, 0x80, 0x0888, 0, 0
    r = harness.call(app, "j(string,uint256)", 64, 2184, 0, bytes.fromhex(''))
    # TODO: verify structural decoding matches expected: 96, 128, 2184, 0, 0
    assert not r.reverted
    # j(string,uint256): 0x40, 0x888, 0 -> 0x60, 0x80, 0x0888, 0, 0
    r = harness.call(app, "j(string,uint256)", 64, 2184, 0)
    # TODO: verify structural decoding matches expected: 96, 128, 2184, 0, 0
    assert not r.reverted

def test_return_string(harness):
    """strings/contracts/return_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/return_string.sol")
    # set(string): 0x20, 5, "Julia" ->
    r = harness.call(app, "set(string)", 32, 5, bytes.fromhex('4a756c6961'))
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
    assert r.abi_return == 4088574885656074156409271147257688673164889634092489131729182745150422515712

def test_unicode_escapes(harness):
    """strings/contracts/unicode_escapes.sol"""
    app = harness.compile_and_deploy("strings/contracts/unicode_escapes.sol")
    # oneByteUTF8() -> 0x20, 7, "aaa$aaa"
    r = harness.call(app, "oneByteUTF8()")
    assert r.abi_return == 'aaa$aaa'
    # twoBytesUTF8() -> 0x20, 8, "aaa\xc2\xa2aaa"
    r = harness.call(app, "twoBytesUTF8()")
    assert r.abi_return == 'aaa\\xc2\\xa2aaa'
    # threeBytesUTF8() -> 0x20, 9, "aaa\xe2\x82\xacaaa"
    r = harness.call(app, "threeBytesUTF8()")
    assert r.abi_return == 'aaa\\xe2\\x82\\xacaaa'
    # combined() -> 0x20, 6, "$\xc2\xa2\xe2\x82\xac"
    r = harness.call(app, "combined()")
    assert r.abi_return == '$\\xc2\\xa2\\xe2\\x82\\xac'

def test_unicode_string(harness):
    """strings/contracts/unicode_string.sol"""
    app = harness.compile_and_deploy("strings/contracts/unicode_string.sol")
    # f() -> 0x20, 0x14, "\xf0\x9f\x98\x83, \xf0\x9f\x98\xad, and \xf0\x9f\x98\x88"
    r = harness.call(app, "f()")
    assert r.abi_return == '\\xf0\\x9f\\x98\\x83, \\xf0\\x9f\\x98\\xad, and \\xf0\\x9f\\x98\\x88'
    # g() -> 0x20, 0x14, "\xf0\x9f\x98\x83, \xf0\x9f\x98\xad, and \xf0\x9f\x98\x88"
    r = harness.call(app, "g()")
    assert r.abi_return == '\\xf0\\x9f\\x98\\x83, \\xf0\\x9f\\x98\\xad, and \\xf0\\x9f\\x98\\x88'
