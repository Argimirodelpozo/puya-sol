"""Auto-generated tests for the literals category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_denominations(harness):
    """literals/denominations.sol"""
    app = harness.compile_and_deploy("literals/denominations.sol")
    # f() -> 1000000001000000001
    r = harness.call(app, "f()")
    assert r.abi_return == 1000000001000000001

def test_denominations_in_array_sizes(harness):
    """literals/denominations_in_array_sizes.sol"""
    app = harness.compile_and_deploy("literals/denominations_in_array_sizes.sol")
    # lengths() -> 2, 2000000000, 2000000000000000000, 2, 120, 7200, 172800, 1209600
    r = harness.call(app, "lengths()")
    # TODO: verify structural decoding matches expected: 2, 2000000000, 2000000000000000000, 2, 120, 7200, 172800, 1209600
    assert not r.reverted

def test_escape(harness):
    """literals/escape.sol"""
    app = harness.compile_and_deploy("literals/escape.sol")
    # f() -> 2, 0x5c00000000000000000000000000000000000000000000000000000000000000, 0x5c00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (2, 41612782069660507730345822737497216884768900739214577701680069252843780964352, 41612782069660507730345822737497216884768900739214577701680069252843780964352)

def test_ether(harness):
    """literals/ether.sol"""
    app = harness.compile_and_deploy("literals/ether.sol")
    # f() -> 1000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 1000000000000000000

def test_fractional_denominations(harness):
    """literals/fractional_denominations.sol"""
    app = harness.compile_and_deploy("literals/fractional_denominations.sol")
    # g() -> 1500000000
    r = harness.call(app, "g()")
    assert r.abi_return == 1500000000
    # e() -> 1500000000000000000
    r = harness.call(app, "e()")
    assert r.abi_return == 1500000000000000000
    # m() -> 90
    r = harness.call(app, "m()")
    assert r.abi_return == 90
    # h() -> 5400
    r = harness.call(app, "h()")
    assert r.abi_return == 5400
    # d() -> 129600
    r = harness.call(app, "d()")
    assert r.abi_return == 129600
    # w() -> 907200
    r = harness.call(app, "w()")
    assert r.abi_return == 907200

def test_gwei(harness):
    """literals/gwei.sol"""
    app = harness.compile_and_deploy("literals/gwei.sol")
    # f() -> 1000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 1000000000

def test_hex_string_with_non_printable_characters(harness):
    """literals/hex_string_with_non_printable_characters.sol"""
    app = harness.compile_and_deploy("literals/hex_string_with_non_printable_characters.sol")
    # f() -> 0x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
    r = harness.call(app, "f()")
    assert r.abi_return == 1780731860627700044960722568376592200742329637303199754547598369979440671

def test_hex_string_with_underscore(harness):
    """literals/hex_string_with_underscore.sol"""
    app = harness.compile_and_deploy("literals/hex_string_with_underscore.sol")
    # f() -> 32, 5, left(0x123456789A)
    r = harness.call(app, "f()")
    # TODO: verify expected: 32 | 5 | left(0x123456789A)
    assert not r.reverted

def test_scientific_notation(harness):
    """literals/scientific_notation.sol"""
    app = harness.compile_and_deploy("literals/scientific_notation.sol")
    # f() -> 20000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 20000000000
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2
    # h() -> 25
    r = harness.call(app, "h()")
    assert r.abi_return == 25
    # i() -> -20000000000
    r = harness.call(app, "i()")
    assert r.abi_return == -20000000000
    # j() -> -2
    r = harness.call(app, "j()")
    assert r.abi_return == -2
    # k() -> -25
    r = harness.call(app, "k()")
    assert r.abi_return == -25

def test_ternary_operator_with_literal_types_overflow(harness):
    """literals/ternary_operator_with_literal_types_overflow.sol"""
    app = harness.compile_and_deploy("literals/ternary_operator_with_literal_types_overflow.sol")
    # g() -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_wei(harness):
    """literals/wei.sol"""
    app = harness.compile_and_deploy("literals/wei.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert r.abi_return == 1
