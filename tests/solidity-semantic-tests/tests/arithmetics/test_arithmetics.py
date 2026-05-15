"""Tests for the arithmetics category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_addmod_mulmod(harness):
    """arithmetics/contracts/addmod_mulmod.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/addmod_mulmod.sol")
    # test() -> 0
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 0

def test_addmod_mulmod_zero(harness):
    """arithmetics/contracts/addmod_mulmod_zero.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/addmod_mulmod_zero.sol")
    # f(uint256): 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(uint256)", 0, expect_revert=True)
    assert r.reverted
    # g(uint256): 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(uint256)", 0, expect_revert=True)
    assert r.reverted
    # h() -> 2
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 2

def test_block_inside_unchecked(harness):
    """arithmetics/contracts/block_inside_unchecked.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/block_inside_unchecked.sol")
    # f() -> 0x00
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_check_var_init(harness):
    """arithmetics/contracts/check_var_init.sol"""
    app = harness.compile_and_deploy('arithmetics/contracts/check_var_init.sol')
    r = harness.call(app, 'f(, expect_revert=True)')
    assert r.reverted

def test_checked_add_v1(harness):
    """arithmetics/contracts/checked_add_v1.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/checked_add_v1.sol")
    # f(uint16,uint16): 65534, 0 -> 0xfffe
    r = harness.call(app, "f(uint16,uint16)", 65534, 0)
    assert as_int(r.abi_return) == 65534
    # f(uint16,uint16): 65536, 0 -> 0x00
    r = harness.call(app, "f(uint16,uint16)", 65536, 0)
    assert as_int(r.abi_return) == 0
    # f(uint16,uint16): 65535, 0 -> 0xffff
    r = harness.call(app, "f(uint16,uint16)", 65535, 0)
    assert as_int(r.abi_return) == 65535
    # f(uint16,uint16): 65535, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint16,uint16)", 65535, 1, expect_revert=True)
    assert r.reverted

def test_checked_add_v2(harness):
    """arithmetics/contracts/checked_add_v2.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/checked_add_v2.sol")
    # f(uint16,uint16): 65534, 0 -> 0xfffe
    r = harness.call(app, "f(uint16,uint16)", 65534, 0)
    assert as_int(r.abi_return) == 65534
    # f(uint16,uint16): 65536, 0 -> FAILURE
    r = harness.call(app, "f(uint16,uint16)", 65536, 0, expect_revert=True)
    assert r.reverted
    # f(uint16,uint16): 65535, 0 -> 0xffff
    r = harness.call(app, "f(uint16,uint16)", 65535, 0)
    assert as_int(r.abi_return) == 65535
    # f(uint16,uint16): 65535, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint16,uint16)", 65535, 1, expect_revert=True)
    assert r.reverted

def test_checked_called_by_unchecked(harness):
    """arithmetics/contracts/checked_called_by_unchecked.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/checked_called_by_unchecked.sol")
    # f(uint16,uint16,uint16): 0xe000, 0xe500, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint16,uint16,uint16)", 57344, 58624, 2, expect_revert=True)
    assert r.reverted
    # f(uint16,uint16,uint16): 0xe000, 0x1000, 0x1000 -> 0x00
    r = harness.call(app, "f(uint16,uint16,uint16)", 57344, 4096, 4096)
    assert as_int(r.abi_return) == 0

def test_checked_modifier_called_by_unchecked(harness):
    """arithmetics/contracts/checked_modifier_called_by_unchecked.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/checked_modifier_called_by_unchecked.sol")
    # f(uint16,uint16,uint16): 0xe000, 0xe500, 2 -> 58626
    r = harness.call(app, "f(uint16,uint16,uint16)", 57344, 58624, 2)
    assert as_int(r.abi_return) == 58626
    # f(uint16,uint16,uint16): 0x1000, 0xe500, 0xe000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint16,uint16,uint16)", 4096, 58624, 57344, expect_revert=True)
    assert r.reverted

def test_divisiod_by_zero(harness):
    """arithmetics/contracts/divisiod_by_zero.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/divisiod_by_zero.sol")
    # div(uint256,uint256): 7, 2 -> 3
    r = harness.call(app, "div(uint256,uint256)", 7, 2)
    assert as_int(r.abi_return) == 3
    # div(uint256,uint256): 7, 0 -> FAILURE, hex"4e487b71", 0x12 # throws #
    r = harness.call(app, "div(uint256,uint256)", 7, 0, expect_revert=True)
    assert r.reverted
    # mod(uint256,uint256): 7, 2 -> 1
    r = harness.call(app, "mod(uint256,uint256)", 7, 2)
    assert as_int(r.abi_return) == 1
    # mod(uint256,uint256): 7, 0 -> FAILURE, hex"4e487b71", 0x12 # throws #
    r = harness.call(app, "mod(uint256,uint256)", 7, 0, expect_revert=True)
    assert r.reverted

def test_exp_associativity(harness):
    """arithmetics/contracts/exp_associativity.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/exp_associativity.sol")
    # test_hardcode1(uint256,uint256,uint256): 2, 3, 4 -> 2417851639229258349412352
    r = harness.call(app, "test_hardcode1(uint256,uint256,uint256)", 2, 3, 4)
    assert as_int(r.abi_return) == 2417851639229258349412352
    # test_hardcode2(uint256,uint256,uint256,uint256): 3, 2, 2, 2 -> 43046721
    r = harness.call(app, "test_hardcode2(uint256,uint256,uint256,uint256)", 3, 2, 2, 2)
    assert as_int(r.abi_return) == 43046721
    # test_invariant(uint256,uint256,uint256): 2, 3, 4 -> true
    r = harness.call(app, "test_invariant(uint256,uint256,uint256)", 2, 3, 4)
    assert bool(as_int(r.abi_return)) is True
    # test_invariant(uint256,uint256,uint256): 3, 4, 2 -> true
    r = harness.call(app, "test_invariant(uint256,uint256,uint256)", 3, 4, 2)
    assert bool(as_int(r.abi_return)) is True
    # test_literal_mix(uint256,uint256): 2, 3 -> true
    r = harness.call(app, "test_literal_mix(uint256,uint256)", 2, 3)
    assert bool(as_int(r.abi_return)) is True
    # test_other_operators(uint256,uint256): 2, 4 -> true
    r = harness.call(app, "test_other_operators(uint256,uint256)", 2, 4)
    assert bool(as_int(r.abi_return)) is True

def test_signed_mod(harness):
    """arithmetics/contracts/signed_mod.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/signed_mod.sol")
    # f(int256,int256): 7, 5 -> 2
    r = harness.call(app, "f(int256,int256)", 7, 5)
    assert as_int(r.abi_return) == 2
    # f(int256,int256): 7, -5 -> 2
    r = harness.call(app, "f(int256,int256)", 7, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert as_int(r.abi_return) == 2
    # f(int256,int256): -7, 5 -> -2
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 5)
    assert as_int(r.abi_return) in (-2, 115792089237316195423570985008687907853269984665640564039457584007913129639934)
    # f(int256,int256): -7, 5 -> -2
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 5)
    assert as_int(r.abi_return) in (-2, 115792089237316195423570985008687907853269984665640564039457584007913129639934)
    # f(int256,int256): -5, -5 -> 0
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert as_int(r.abi_return) == 0
    # g(bool): true -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(bool)", True, expect_revert=True)
    assert r.reverted
    # g(bool): false -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "g(bool)", False)
    assert as_int(r.abi_return) in (-57896044618658097711785492504343953926634992332820282019728792003956564819968, 57896044618658097711785492504343953926634992332820282019728792003956564819968)

def test_unchecked_called_by_checked(harness):
    """arithmetics/contracts/unchecked_called_by_checked.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/unchecked_called_by_checked.sol")
    # f(uint16): 7 -> 0x0207
    r = harness.call(app, "f(uint16)", 7)
    assert as_int(r.abi_return) == 519
    # f(uint16): 0xffff -> 511
    r = harness.call(app, "f(uint16)", 65535)
    assert as_int(r.abi_return) == 511
    # f(uint16): 0xfeff -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint16)", 65279, expect_revert=True)
    assert r.reverted

def test_unchecked_div_by_zero(harness):
    """arithmetics/contracts/unchecked_div_by_zero.sol"""
    app = harness.compile_and_deploy("arithmetics/contracts/unchecked_div_by_zero.sol")
    # div(uint256,uint256): 7, 2 -> 3
    r = harness.call(app, "div(uint256,uint256)", 7, 2)
    assert as_int(r.abi_return) == 3
    # div(uint256,uint256): 7, 0 -> FAILURE, hex"4e487b71", 0x12 # throws #
    r = harness.call(app, "div(uint256,uint256)", 7, 0, expect_revert=True)
    assert r.reverted
    # mod(uint256,uint256): 7, 2 -> 1
    r = harness.call(app, "mod(uint256,uint256)", 7, 2)
    assert as_int(r.abi_return) == 1
    # mod(uint256,uint256): 7, 0 -> FAILURE, hex"4e487b71", 0x12 # throws #
    r = harness.call(app, "mod(uint256,uint256)", 7, 0, expect_revert=True)
    assert r.reverted
