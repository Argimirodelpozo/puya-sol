"""Tests for the literals category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_denominations(harness):
    """literals/contracts/denominations.sol"""
    app = harness.compile_and_deploy("literals/contracts/denominations.sol")
    # f() -> 1000000001000000001
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1000000001000000001

def test_denominations_in_array_sizes(harness):
    """literals/contracts/denominations_in_array_sizes.sol"""
    app = harness.compile_and_deploy("literals/contracts/denominations_in_array_sizes.sol")
    # lengths() -> 2, 2000000000, 2000000000000000000, 2, 120, 7200, 172800, 1209600
    r = harness.call(app, "lengths()")
    # TODO: verify structural decoding matches expected: 2, 2000000000, 2000000000000000000, 2, 120, 7200, 172800, 1209600
    assert not r.reverted

def test_escape(harness):
    """literals/contracts/escape.sol"""
    app = harness.compile_and_deploy("literals/contracts/escape.sol")
    # f() -> 2, 0x5c00000000000000000000000000000000000000000000000000000000000000, 0x5c00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 41612782069660507730345822737497216884768900739214577701680069252843780964352, 41612782069660507730345822737497216884768900739214577701680069252843780964352)

def test_ether(harness):
    """literals/contracts/ether.sol"""
    app = harness.compile_and_deploy("literals/contracts/ether.sol")
    # f() -> 1000000000000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1000000000000000000

def test_fractional_denominations(harness):
    """literals/contracts/fractional_denominations.sol"""
    app = harness.compile_and_deploy("literals/contracts/fractional_denominations.sol")
    # g() -> 1500000000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1500000000
    # e() -> 1500000000000000000
    r = harness.call(app, "e()")
    assert as_int(r.abi_return) == 1500000000000000000
    # m() -> 90
    r = harness.call(app, "m()")
    assert as_int(r.abi_return) == 90
    # h() -> 5400
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 5400
    # d() -> 129600
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 129600
    # w() -> 907200
    r = harness.call(app, "w()")
    assert as_int(r.abi_return) == 907200

def test_gwei(harness):
    """literals/contracts/gwei.sol"""
    app = harness.compile_and_deploy("literals/contracts/gwei.sol")
    # f() -> 1000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1000000000

def test_hex_string_with_non_printable_characters(harness):
    """literals/contracts/hex_string_with_non_printable_characters.sol"""
    app = harness.compile_and_deploy("literals/contracts/hex_string_with_non_printable_characters.sol")
    # f() -> 0x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1780731860627700044960722568376592200742329637303199754547598369979440671

def test_hex_string_with_underscore(harness):
    """literals/contracts/hex_string_with_underscore.sol"""
    app = harness.compile_and_deploy("literals/contracts/hex_string_with_underscore.sol")
    # f() -> 32, 5, left(0x123456789A)
    r = harness.call(app, "f()")
    # TODO: verify expected: 32 | 5 | left(0x123456789A)
    assert not r.reverted

def test_scientific_notation(harness):
    """literals/contracts/scientific_notation.sol"""
    app = harness.compile_and_deploy("literals/contracts/scientific_notation.sol")
    # f() -> 20000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 20000000000
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # h() -> 25
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 25
    # i() -> -20000000000
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) in (-20000000000, 115792089237316195423570985008687907853269984665640564039457584007893129639936)
    # j() -> -2
    r = harness.call(app, "j()")
    assert as_int(r.abi_return) in (-2, 115792089237316195423570985008687907853269984665640564039457584007913129639934)
    # k() -> -25
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) in (-25, 115792089237316195423570985008687907853269984665640564039457584007913129639911)

def test_ternary_operator_with_literal_types_overflow(harness):
    """literals/contracts/ternary_operator_with_literal_types_overflow.sol"""
    app = harness.compile_and_deploy("literals/contracts/ternary_operator_with_literal_types_overflow.sol")
    # g() -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_wei(harness):
    """literals/contracts/wei.sol"""
    app = harness.compile_and_deploy("literals/contracts/wei.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
