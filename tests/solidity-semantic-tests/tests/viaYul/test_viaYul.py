"""Auto-generated tests for the viaYul category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_assert_(harness):
    """viaYul/contracts/assert.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/assert.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True
    # f(bool): false -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # fail() -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "fail()", expect_revert=True)
    assert r.reverted
    # succeed() -> true
    r = harness.call(app, "succeed()")
    assert r.abi_return is True

def test_assert_and_require(harness):
    """viaYul/contracts/assert_and_require.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/assert_and_require.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True
    # f(bool): false -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # f2(bool): true -> true
    r = harness.call(app, "f2(bool)", True)
    assert r.abi_return is True
    # f2(bool): false -> FAILURE
    r = harness.call(app, "f2(bool)", False, expect_revert=True)
    assert r.reverted

def test_assign_tuple_from_function_call(harness):
    """viaYul/contracts/assign_tuple_from_function_call.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/assign_tuple_from_function_call.sol")
    # g() -> 3, 2, 1
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (3, 2, 1)
    # h() -> 3
    r = harness.call(app, "h()")
    assert r.abi_return == 3

def test_comparison(harness):
    """viaYul/contracts/comparison.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/comparison.sol")
    # f(address): 0x1234 -> false
    r = harness.call(app, "f(address)", 4660)
    assert r.abi_return is False
    # f(address): 0x00 -> true
    r = harness.call(app, "f(address)", 0)
    assert r.abi_return is True
    # g() -> true
    r = harness.call(app, "g()")
    assert r.abi_return is True
    # lt(uint256,uint256): 4, 5 -> true
    r = harness.call(app, "lt(uint256,uint256)", 4, 5)
    assert r.abi_return is True
    # lt(uint256,uint256): 5, 5 -> false
    r = harness.call(app, "lt(uint256,uint256)", 5, 5)
    assert r.abi_return is False
    # lt(uint256,uint256): 6, 5 -> false
    r = harness.call(app, "lt(uint256,uint256)", 6, 5)
    assert r.abi_return is False
    # gt(uint256,uint256): 4, 5 -> false
    r = harness.call(app, "gt(uint256,uint256)", 4, 5)
    assert r.abi_return is False
    # gt(uint256,uint256): 5, 5 -> false
    r = harness.call(app, "gt(uint256,uint256)", 5, 5)
    assert r.abi_return is False
    # gt(uint256,uint256): 6, 5 -> true
    r = harness.call(app, "gt(uint256,uint256)", 6, 5)
    assert r.abi_return is True
    # lte(uint256,uint256): 4, 5 -> true
    r = harness.call(app, "lte(uint256,uint256)", 4, 5)
    assert r.abi_return is True
    # lte(uint256,uint256): 5, 5 -> true
    r = harness.call(app, "lte(uint256,uint256)", 5, 5)
    assert r.abi_return is True
    # lte(uint256,uint256): 6, 5 -> false
    r = harness.call(app, "lte(uint256,uint256)", 6, 5)
    assert r.abi_return is False
    # gte(uint256,uint256): 4, 5 -> false
    r = harness.call(app, "gte(uint256,uint256)", 4, 5)
    assert r.abi_return is False
    # gte(uint256,uint256): 5, 5 -> true
    r = harness.call(app, "gte(uint256,uint256)", 5, 5)
    assert r.abi_return is True
    # gte(uint256,uint256): 6, 5 -> true
    r = harness.call(app, "gte(uint256,uint256)", 6, 5)
    assert r.abi_return is True
    # eq(uint256,uint256): 4, 5 -> false
    r = harness.call(app, "eq(uint256,uint256)", 4, 5)
    assert r.abi_return is False
    # eq(uint256,uint256): 5, 5 -> true
    r = harness.call(app, "eq(uint256,uint256)", 5, 5)
    assert r.abi_return is True
    # eq(uint256,uint256): 6, 5 -> false
    r = harness.call(app, "eq(uint256,uint256)", 6, 5)
    assert r.abi_return is False
    # neq(uint256,uint256): 4, 5 -> true
    r = harness.call(app, "neq(uint256,uint256)", 4, 5)
    assert r.abi_return is True
    # neq(uint256,uint256): 5, 5 -> false
    r = harness.call(app, "neq(uint256,uint256)", 5, 5)
    assert r.abi_return is False
    # neq(uint256,uint256): 6, 5 -> true
    r = harness.call(app, "neq(uint256,uint256)", 6, 5)
    assert r.abi_return is True
    # slt(int256,int256): -1, 0 -> true
    r = harness.call(app, "slt(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return is True
    # slt(int256,int256): 0, 0 -> false
    r = harness.call(app, "slt(int256,int256)", 0, 0)
    assert r.abi_return is False
    # slt(int256,int256): 1, 0 -> false
    r = harness.call(app, "slt(int256,int256)", 1, 0)
    assert r.abi_return is False
    # sgt(int256,int256): -1, 0 -> false
    r = harness.call(app, "sgt(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return is False
    # sgt(int256,int256): 0, 0 -> false
    r = harness.call(app, "sgt(int256,int256)", 0, 0)
    assert r.abi_return is False
    # sgt(int256,int256): 1, 0 -> true
    r = harness.call(app, "sgt(int256,int256)", 1, 0)
    assert r.abi_return is True
    # slte(int256,int256): -1, 0 -> true
    r = harness.call(app, "slte(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return is True
    # slte(int256,int256): 0, 0 -> true
    r = harness.call(app, "slte(int256,int256)", 0, 0)
    assert r.abi_return is True
    # slte(int256,int256): 1, 0 -> false
    r = harness.call(app, "slte(int256,int256)", 1, 0)
    assert r.abi_return is False
    # sgte(int256,int256): -1, 0 -> false
    r = harness.call(app, "sgte(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return is False
    # sgte(int256,int256): 0, 0 -> true
    r = harness.call(app, "sgte(int256,int256)", 0, 0)
    assert r.abi_return is True
    # sgte(int256,int256): 1, 0 -> true
    r = harness.call(app, "sgte(int256,int256)", 1, 0)
    assert r.abi_return is True

def test_comparison_functions(harness):
    """viaYul/contracts/comparison_functions.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/comparison_functions.sol")
    # equal() -> true, false, false
    r = harness.call(app, "equal()")
    assert tuple(r.abi_return) == (True, False, False)
    # unequal() -> false, true, true
    r = harness.call(app, "unequal()")
    assert tuple(r.abi_return) == (False, True, True)

def test_copy_struct_invalid_ir_bug(harness):
    """viaYul/contracts/copy_struct_invalid_ir_bug.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/copy_struct_invalid_ir_bug.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_define_tuple_from_function_call(harness):
    """viaYul/contracts/define_tuple_from_function_call.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/define_tuple_from_function_call.sol")
    # g() -> 3, 2, 1
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (3, 2, 1)
    # h() -> 3
    r = harness.call(app, "h()")
    assert r.abi_return == 3

def test_delete(harness):
    """viaYul/contracts/delete.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/delete.sol")
    # call_deleted_internal_func() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "call_deleted_internal_func()", expect_revert=True)
    assert r.reverted
    # call_internal_func() -> true
    r = harness.call(app, "call_internal_func()")
    assert r.abi_return is True

def test_detect_add_overflow(harness):
    """viaYul/contracts/detect_add_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_add_overflow.sol")
    # f(uint256,uint256): 5, 6 -> 11
    r = harness.call(app, "f(uint256,uint256)", 5, 6)
    assert r.abi_return == 11
    # f(uint256,uint256): -2, 1 -> -1
    r = harness.call(app, "f(uint256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -1
    # f(uint256,uint256): -2, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 2, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 2, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 128, 64 -> 192
    r = harness.call(app, "g(uint8,uint8)", 128, 64)
    assert r.abi_return == 192
    # g(uint8,uint8): 128, 127 -> 255
    r = harness.call(app, "g(uint8,uint8)", 128, 127)
    assert r.abi_return == 255
    # g(uint8,uint8): 128, 128 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 128, 128, expect_revert=True)
    assert r.reverted

def test_detect_add_overflow_signed(harness):
    """viaYul/contracts/detect_add_overflow_signed.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_add_overflow_signed.sol")
    # f(int256,int256): 5, 6 -> 11
    r = harness.call(app, "f(int256,int256)", 5, 6)
    assert r.abi_return == 11
    # f(int256,int256): -2, 1 -> -1
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -1
    # f(int256,int256): -2, 2 -> 0
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2)
    assert r.abi_return == 0
    # f(int256,int256): 2, -2 -> 0
    r = harness.call(app, "f(int256,int256)", 2, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 0
    # f(int256,int256): -5, -6 -> -11
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa)
    assert r.abi_return == -11
    # f(int256,int256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0, 0x0F -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    r = harness.call(app, "f(int256,int256)", 0x7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0, 15)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819967
    # f(int256,int256): 0x0F, 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    r = harness.call(app, "f(int256,int256)", 15, 0x7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819967
    # f(int256,int256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 1, 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 1, 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000001, -1 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000001, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): -1, 0x8000000000000000000000000000000000000000000000000000000000000001 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0x8000000000000000000000000000000000000000000000000000000000000001)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000000, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -1, 0x8000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0x8000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 5, 6 -> 11
    r = harness.call(app, "g(int8,int8)", 5, 6)
    assert r.abi_return == 11
    # g(int8,int8): -2, 1 -> -1
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -1
    # g(int8,int8): -2, 2 -> 0
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2)
    assert r.abi_return == 0
    # g(int8,int8): 2, -2 -> 0
    r = harness.call(app, "g(int8,int8)", 2, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 0
    # g(int8,int8): -5, -6 -> -11
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa)
    assert r.abi_return == -11
    # g(int8,int8): 126, 1 -> 127
    r = harness.call(app, "g(int8,int8)", 126, 1)
    assert r.abi_return == 127
    # g(int8,int8): 1, 126 -> 127
    r = harness.call(app, "g(int8,int8)", 1, 126)
    assert r.abi_return == 127
    # g(int8,int8): 127, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 127, 1, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 1, 127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 1, 127, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -127, -1 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -128
    # g(int8,int8): -1, -127 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81)
    assert r.abi_return == -128
    # g(int8,int8): -127, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -2, -127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -128, 0 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 0)
    assert r.abi_return == -128
    # g(int8,int8): 0, -128 -> -128
    r = harness.call(app, "g(int8,int8)", 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80)
    assert r.abi_return == -128

def test_detect_div_overflow(harness):
    """viaYul/contracts/detect_div_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_div_overflow.sol")
    # f(uint256,uint256): 10, 3 -> 3
    r = harness.call(app, "f(uint256,uint256)", 10, 3)
    assert r.abi_return == 3
    # f(uint256,uint256): 1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(uint256,uint256)", 1, 0, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0, 1 -> 0
    r = harness.call(app, "f(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # g(int8,int8): -10, 3 -> -3
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 3)
    assert r.abi_return == -3
    # g(int8,int8): -10, -3 -> 3
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd)
    assert r.abi_return == 3
    # g(int8,int8): -10, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 0, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -128, 1 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 1)
    assert r.abi_return == -128
    # g(int8,int8): -128, -2 -> 64
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 64
    # g(int8,int8): -128, 2 -> -64
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 2)
    assert r.abi_return == -64
    # g(int8,int8): -128, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -127, -1 -> 127
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 127
    # h(uint256,uint256): 0x8000000000000000000000000000000000000000000000000000000000000000, -1 -> 0
    r = harness.call(app, "h(uint256,uint256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 0

def test_detect_mod_zero(harness):
    """viaYul/contracts/detect_mod_zero.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_mod_zero.sol")
    # f(uint256,uint256): 10, 3 -> 1
    r = harness.call(app, "f(uint256,uint256)", 10, 3)
    assert r.abi_return == 1
    # f(uint256,uint256): 10, 2 -> 0
    r = harness.call(app, "f(uint256,uint256)", 10, 2)
    assert r.abi_return == 0
    # f(uint256,uint256): 11, 2 -> 1
    r = harness.call(app, "f(uint256,uint256)", 11, 2)
    assert r.abi_return == 1
    # f(uint256,uint256): 2, 2 -> 0
    r = harness.call(app, "f(uint256,uint256)", 2, 2)
    assert r.abi_return == 0
    # f(uint256,uint256): 1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(uint256,uint256)", 1, 0, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0, 1 -> 0
    r = harness.call(app, "f(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # g(uint8,uint8): 10, 3 -> 1
    r = harness.call(app, "g(uint8,uint8)", 10, 3)
    assert r.abi_return == 1
    # g(uint8,uint8): 10, 2 -> 0
    r = harness.call(app, "g(uint8,uint8)", 10, 2)
    assert r.abi_return == 0
    # g(uint8,uint8): 11, 2 -> 1
    r = harness.call(app, "g(uint8,uint8)", 11, 2)
    assert r.abi_return == 1
    # g(uint8,uint8): 2, 2 -> 0
    r = harness.call(app, "g(uint8,uint8)", 2, 2)
    assert r.abi_return == 0
    # g(uint8,uint8): 1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(uint8,uint8)", 1, 0, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(uint8,uint8)", 0, 0, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0, 1 -> 0
    r = harness.call(app, "g(uint8,uint8)", 0, 1)
    assert r.abi_return == 0

def test_detect_mod_zero_signed(harness):
    """viaYul/contracts/detect_mod_zero_signed.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_mod_zero_signed.sol")
    # f(int256,int256): 10, 3 -> 1
    r = harness.call(app, "f(int256,int256)", 10, 3)
    assert r.abi_return == 1
    # f(int256,int256): 10, 2 -> 0
    r = harness.call(app, "f(int256,int256)", 10, 2)
    assert r.abi_return == 0
    # f(int256,int256): 11, 2 -> 1
    r = harness.call(app, "f(int256,int256)", 11, 2)
    assert r.abi_return == 1
    # f(int256,int256): -10, 3 -> -1
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 3)
    assert r.abi_return == -1
    # f(int256,int256): 10, -3 -> 1
    r = harness.call(app, "f(int256,int256)", 10, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd)
    assert r.abi_return == 1
    # f(int256,int256): -10, -3 -> -1
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd)
    assert r.abi_return == -1
    # f(int256,int256): 2, 2 -> 0
    r = harness.call(app, "f(int256,int256)", 2, 2)
    assert r.abi_return == 0
    # f(int256,int256): 1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(int256,int256)", 1, 0, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "f(int256,int256)", 0, 0, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0, 1 -> 0
    r = harness.call(app, "f(int256,int256)", 0, 1)
    assert r.abi_return == 0
    # f(int256,int256): 0, -1 -> 0
    r = harness.call(app, "f(int256,int256)", 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 0
    # g(int8,int8): 10, 3 -> 1
    r = harness.call(app, "g(int8,int8)", 10, 3)
    assert r.abi_return == 1
    # g(int8,int8): 10, 2 -> 0
    r = harness.call(app, "g(int8,int8)", 10, 2)
    assert r.abi_return == 0
    # g(int8,int8): 11, 2 -> 1
    r = harness.call(app, "g(int8,int8)", 11, 2)
    assert r.abi_return == 1
    # g(int8,int8): -10, 3 -> -1
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 3)
    assert r.abi_return == -1
    # g(int8,int8): 10, -3 -> 1
    r = harness.call(app, "g(int8,int8)", 10, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd)
    assert r.abi_return == 1
    # g(int8,int8): -10, -3 -> -1
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd)
    assert r.abi_return == -1
    # g(int8,int8): 2, 2 -> 0
    r = harness.call(app, "g(int8,int8)", 2, 2)
    assert r.abi_return == 0
    # g(int8,int8): 1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(int8,int8)", 1, 0, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -1, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 0, 0 -> FAILURE, hex"4e487b71", 0x12
    r = harness.call(app, "g(int8,int8)", 0, 0, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 0, 1 -> 0
    r = harness.call(app, "g(int8,int8)", 0, 1)
    assert r.abi_return == 0
    # g(int8,int8): 0, -1 -> 0
    r = harness.call(app, "g(int8,int8)", 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 0
    # g(int8,int8): -128, -128 -> 0
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80)
    assert r.abi_return == 0
    # g(int8,int8): -128, 127 -> -1
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 127)
    assert r.abi_return == -1

def test_detect_mul_overflow(harness):
    """viaYul/contracts/detect_mul_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_mul_overflow.sol")
    # f(uint256,uint256): 5, 6 -> 30
    r = harness.call(app, "f(uint256,uint256)", 5, 6)
    assert r.abi_return == 30
    # f(uint256,uint256): -1, 1 -> -1
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1)
    assert r.abi_return == -1
    # f(uint256,uint256): -1, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0x8000000000000000000000000000000000000000000000000000000000000000, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 2, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> -2
    r = harness.call(app, "f(uint256,uint256)", 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    assert r.abi_return == -2
    # f(uint256,uint256): 2, 0x8000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 2, 0x8000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 2, 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> -2
    r = harness.call(app, "f(uint256,uint256)", 2, 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -2
    # f(uint256,uint256): 0x0100000000000000000000000000000000, 0x0100000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0x100000000000000000000000000000000, 0x100000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0x0100000000000000000000000000000000, 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF00000000000000000000000000000000
    r = harness.call(app, "f(uint256,uint256)", 0x100000000000000000000000000000000, 0xffffffffffffffffffffffffffffffff)
    assert r.abi_return == 115792089237316195423570985008687907852929702298719625575994209400481361428480
    # f(uint256,uint256): 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 0x0100000000000000000000000000000000 -> 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF00000000000000000000000000000000
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffff, 0x100000000000000000000000000000000)
    assert r.abi_return == 115792089237316195423570985008687907852929702298719625575994209400481361428480
    # f(uint256,uint256): 0x0100000000000000000000000000000001, 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> -1
    r = harness.call(app, "f(uint256,uint256)", 0x100000000000000000000000000000001, 0xffffffffffffffffffffffffffffffff)
    assert r.abi_return == -1
    # f(uint256,uint256): 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 0x0100000000000000000000000000000001 -> -1
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffff, 0x100000000000000000000000000000001)
    assert r.abi_return == -1
    # f(uint256,uint256): 0x0100000000000000000000000000000002, 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0x100000000000000000000000000000002, 0xffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): 0x00FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 0x0100000000000000000000000000000002 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffff, 0x100000000000000000000000000000002, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256): -1, 0 -> 0
    r = harness.call(app, "f(uint256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return == 0
    # f(uint256,uint256): 0, -1 -> 0
    r = harness.call(app, "f(uint256,uint256)", 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 0
    # g(uint8,uint8): 5, 6 -> 30
    r = harness.call(app, "g(uint8,uint8)", 5, 6)
    assert r.abi_return == 30
    # g(uint8,uint8): 0x80, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 128, 2, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0x7F, 2 -> 254
    r = harness.call(app, "g(uint8,uint8)", 127, 2)
    assert r.abi_return == 254
    # g(uint8,uint8): 2, 0x7F -> 254
    r = harness.call(app, "g(uint8,uint8)", 2, 127)
    assert r.abi_return == 254
    # g(uint8,uint8): 0x10, 0x10 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 16, 16, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0x0F, 0x11 -> 0xFF
    r = harness.call(app, "g(uint8,uint8)", 15, 17)
    assert r.abi_return == 255
    # g(uint8,uint8): 0x0F, 0x12 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 15, 18, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0x12, 0x0F -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 18, 15, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 0xFF, 0 -> 0
    r = harness.call(app, "g(uint8,uint8)", 255, 0)
    assert r.abi_return == 0
    # g(uint8,uint8): 0, 0xFF -> 0
    r = harness.call(app, "g(uint8,uint8)", 0, 255)
    assert r.abi_return == 0

def test_detect_mul_overflow_signed(harness):
    """viaYul/contracts/detect_mul_overflow_signed.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_mul_overflow_signed.sol")
    # f(int256,int256): 5, 6 -> 30
    r = harness.call(app, "f(int256,int256)", 5, 6)
    assert r.abi_return == 30
    # f(int256,int256): -1, 1 -> -1
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1)
    assert r.abi_return == -1
    # f(int256,int256): -1, 2 -> -2 # positive, positive #
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    # TODO: verify expected: -2 # positive | positive #
    assert not r.reverted
    # f(int256,int256): 0x3FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE
    r = harness.call(app, "f(int256,int256)", 0x3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819966
    # f(int256,int256): 0x4000000000000000000000000000000000000000000000000000000000000000, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x4000000000000000000000000000000000000000000000000000000000000000, 2, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0x3FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE
    r = harness.call(app, "f(int256,int256)", 2, 0x3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819966
    # f(int256,int256): 2, 0x4000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 2, 0x4000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -1, 0x8000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0x8000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000000, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0x4000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "f(int256,int256)", 2, 0x4000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0x4000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "f(int256,int256)", 2, 0x4000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0x4000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "f(int256,int256)", 2, 0x4000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x4000000000000000000000000000000000000000000000000000000000000000, -2 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0x4000000000000000000000000000000000000000000000000000000000000000, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): 0x4000000000000000000000000000000000000000000000000000000000000001, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x4000000000000000000000000000000000000000000000000000000000000001, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0xC000000000000000000000000000000000000000000000000000000000000000 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 2, 0xc000000000000000000000000000000000000000000000000000000000000000)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): 2, 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "f(int256,int256)", 2, 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "f(int256,int256)", 2, 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 2, 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "f(int256,int256)", 2, 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -2, 0x4000000000000000000000000000000000000000000000000000000000000000 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x4000000000000000000000000000000000000000000000000000000000000000)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): -2, 0x4000000000000000000000000000000000000000000000000000000000000001 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x4000000000000000000000000000000000000000000000000000000000000001, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0xC000000000000000000000000000000000000000000000000000000000000000, 2 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0xc000000000000000000000000000000000000000000000000000000000000000, 2)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "f(int256,int256)", 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "f(int256,int256)", 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0xBFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "f(int256,int256)", 0xbfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0xC000000000000000000000000000000000000000000000000000000000000001, -2 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE
    r = harness.call(app, "f(int256,int256)", 0xc000000000000000000000000000000000000000000000000000000000000001, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819966
    # f(int256,int256): 0xC000000000000000000000000000000000000000000000000000000000000000, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0xc000000000000000000000000000000000000000000000000000000000000000, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -2, 0xC000000000000000000000000000000000000000000000000000000000000001 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xc000000000000000000000000000000000000000000000000000000000000001)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819966
    # f(int256,int256): -2, 0xC000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # small type #
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xc000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -2, 0xC000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # small type #
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xc000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -2, 0xC000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11 # small type #
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xc000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 5, 6 -> 30
    r = harness.call(app, "g(int8,int8)", 5, 6)
    assert r.abi_return == 30
    # g(int8,int8): -1, 1 -> -1
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1)
    assert r.abi_return == -1
    # g(int8,int8): -1, 2 -> -2 # positive, positive #
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    # TODO: verify expected: -2 # positive | positive #
    assert not r.reverted
    # g(int8,int8): 63, 2 -> 126
    r = harness.call(app, "g(int8,int8)", 63, 2)
    assert r.abi_return == 126
    # g(int8,int8): 64, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 64, 2, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, 63 -> 126
    r = harness.call(app, "g(int8,int8)", 2, 63)
    assert r.abi_return == 126
    # g(int8,int8): 2, 64 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "g(int8,int8)", 2, 64, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, 64 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "g(int8,int8)", 2, 64, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, 64 -> FAILURE, hex"4e487b71", 0x11 # positive, negative #
    r = harness.call(app, "g(int8,int8)", 2, 64, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 64, -2 -> -128
    r = harness.call(app, "g(int8,int8)", 64, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == -128
    # g(int8,int8): 65, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 65, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, -64 -> -128
    r = harness.call(app, "g(int8,int8)", 2, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc0)
    assert r.abi_return == -128
    # g(int8,int8): 2, -65 -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "g(int8,int8)", 2, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, -65 -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "g(int8,int8)", 2, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 2, -65 -> FAILURE, hex"4e487b71", 0x11 # negative, positive #
    r = harness.call(app, "g(int8,int8)", 2, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -2, 64 -> -128
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 64)
    assert r.abi_return == -128
    # g(int8,int8): -2, 65 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 65, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -64, 2 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc0, 2)
    assert r.abi_return == -128
    # g(int8,int8): -65, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, 2, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -65, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, 2, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -65, 2 -> FAILURE, hex"4e487b71", 0x11 # negative, negative #
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbf, 2, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -63, -2 -> 126
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc1, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 126
    # g(int8,int8): -64, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc0, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -2, -63 -> 126
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc1)
    assert r.abi_return == 126
    # g(int8,int8): -2, -64 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc0, expect_revert=True)
    assert r.reverted
    # h(int160,int160): -1, 1 -> -1
    r = harness.call(app, "h(int160,int160)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1)
    assert r.abi_return == -1
    # h(int160,int160): 1, -1 -> -1
    r = harness.call(app, "h(int160,int160)", 1, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -1
    # h(int160,int160): -1, 2 -> -2
    r = harness.call(app, "h(int160,int160)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    assert r.abi_return == -2
    # h(int160,int160): 2, -1 -> -2
    r = harness.call(app, "h(int160,int160)", 2, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -2
    # h(int160,int160): -1, 0xFFFFFFFFFFFFFFFFFFFFFFFF8000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffff8000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # h(int160,int160): -1, 0xFFFFFFFFFFFFFFFFFFFFFFFF8000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffff8000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 0xFFFFFFFFFFFFFFFFFFFFFFFF8000000000000000000000000000000000000000, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0xffffffffffffffffffffffff8000000000000000000000000000000000000000, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 0x0000000000000000000000004000000000000000000000000000000000000000, -2 -> 0xFFFFFFFFFFFFFFFFFFFFFFFF8000000000000000000000000000000000000000
    r = harness.call(app, "h(int160,int160)", 0x4000000000000000000000000000000000000000, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 115792089237316195423570985007957157034604533206538721623099442498085163368448
    # h(int160,int160): -2, 0x0000000000000000000000004000000000000000000000000000000000000000 -> 0xFFFFFFFFFFFFFFFFFFFFFFFF8000000000000000000000000000000000000000
    r = harness.call(app, "h(int160,int160)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x4000000000000000000000000000000000000000)
    assert r.abi_return == 115792089237316195423570985007957157034604533206538721623099442498085163368448
    # h(int160,int160): -2, 0x0000000000000000000000004000000000000000000000000000000000000001 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x4000000000000000000000000000000000000001, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 0x0000000000000000000000004000000000000000000000000000000000000001, -2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0x4000000000000000000000000000000000000001, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 0x0000000000000000000000004000000000000000000000000000000000000001, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 0x4000000000000000000000000000000000000001, 2, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 2, 0x0000000000000000000000004000000000000000000000000000000000000001 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 2, 0x4000000000000000000000000000000000000001, expect_revert=True)
    assert r.reverted
    # h(int160,int160): 0x0000000000000000000000003FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 2 -> 0x0000000000000000000000007FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE
    r = harness.call(app, "h(int160,int160)", 0x3fffffffffffffffffffffffffffffffffffffff, 2)
    assert r.abi_return == 730750818665451459101842416358141509827966271486
    # h(int160,int160): 2, 0x0000000000000000000000004000000000000000000000000000000000000001 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "h(int160,int160)", 2, 0x4000000000000000000000000000000000000001, expect_revert=True)
    assert r.reverted

def test_detect_sub_overflow(harness):
    """viaYul/contracts/detect_sub_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_sub_overflow.sol")
    # f(uint256,uint256): 6, 5 -> 1
    r = harness.call(app, "f(uint256,uint256)", 6, 5)
    assert r.abi_return == 1
    # f(uint256,uint256): 6, 6 -> 0
    r = harness.call(app, "f(uint256,uint256)", 6, 6)
    assert r.abi_return == 0
    # f(uint256,uint256): 5, 6 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint256,uint256)", 5, 6, expect_revert=True)
    assert r.reverted
    # g(uint8,uint8): 6, 5 -> 1
    r = harness.call(app, "g(uint8,uint8)", 6, 5)
    assert r.abi_return == 1
    # g(uint8,uint8): 6, 6 -> 0
    r = harness.call(app, "g(uint8,uint8)", 6, 6)
    assert r.abi_return == 0
    # g(uint8,uint8): 5, 6 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint8,uint8)", 5, 6, expect_revert=True)
    assert r.reverted

def test_detect_sub_overflow_signed(harness):
    """viaYul/contracts/detect_sub_overflow_signed.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/detect_sub_overflow_signed.sol")
    # f(int256,int256): 5, 6 -> -1
    r = harness.call(app, "f(int256,int256)", 5, 6)
    assert r.abi_return == -1
    # f(int256,int256): -2, 1 -> -3
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -3
    # f(int256,int256): -2, 2 -> -4
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2)
    assert r.abi_return == -4
    # f(int256,int256): 2, -2 -> 4
    r = harness.call(app, "f(int256,int256)", 2, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 4
    # f(int256,int256): 2, 2 -> 0
    r = harness.call(app, "f(int256,int256)", 2, 2)
    assert r.abi_return == 0
    # f(int256,int256): -5, -6 -> 1
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa)
    assert r.abi_return == 1
    # f(int256,int256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0, -15 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    r = harness.call(app, "f(int256,int256)", 0x7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff1)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819967
    # f(int256,int256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0, -16 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 15, 0x8000000000000000000000000000000000000000000000000000000000000010 -> 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    r = harness.call(app, "f(int256,int256)", 15, 0x8000000000000000000000000000000000000000000000000000000000000010)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819967
    # f(int256,int256): 16, 0x8000000000000000000000000000000000000000000000000000000000000010 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 16, 0x8000000000000000000000000000000000000000000000000000000000000010, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 1, 0x8000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 1, 0x8000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # f(int256,int256): -1, 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): -2, 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000001, 1 -> 0x8000000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000001, 1)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000001, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000001, 2, expect_revert=True)
    assert r.reverted
    # f(int256,int256): 0x8000000000000000000000000000000000000000000000000000000000000000, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int256,int256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 1, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 5, 6 -> -1
    r = harness.call(app, "g(int8,int8)", 5, 6)
    assert r.abi_return == -1
    # g(int8,int8): -2, 1 -> -3
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -3
    # g(int8,int8): -2, 2 -> -4
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2)
    assert r.abi_return == -4
    # g(int8,int8): 2, -2 -> 4
    r = harness.call(app, "g(int8,int8)", 2, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 4
    # g(int8,int8): 2, 2 -> 0
    r = harness.call(app, "g(int8,int8)", 2, 2)
    assert r.abi_return == 0
    # g(int8,int8): -5, -6 -> 1
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa)
    assert r.abi_return == 1
    # g(int8,int8): 126, -1 -> 127
    r = harness.call(app, "g(int8,int8)", 126, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 127
    # g(int8,int8): 1, -126 -> 127
    r = harness.call(app, "g(int8,int8)", 1, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff82)
    assert r.abi_return == 127
    # g(int8,int8): 127, -1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 127, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # g(int8,int8): 1, -127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 1, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -127, 1 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, 1)
    assert r.abi_return == -128
    # g(int8,int8): -1, 127 -> -128
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 127)
    assert r.abi_return == -128
    # g(int8,int8): -127, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81, 2, expect_revert=True)
    assert r.reverted
    # g(int8,int8): -2, 127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int8,int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 127, expect_revert=True)
    assert r.reverted

def test_dirty_calldata_struct(harness):
    """viaYul/contracts/dirty_calldata_struct.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_calldata_struct.sol", via_yul_behavior=True)
    # f((uint16[])): 0x20, 0x20, 0x01, 0x0180 -> true
    r = harness.call(app, "f((uint16[]))", 32, 32, 1, 384)
    assert r.abi_return is True

def test_dirty_memory_dynamic_array(harness):
    """viaYul/contracts/dirty_memory_dynamic_array.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_memory_dynamic_array.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_dirty_memory_int32(harness):
    """viaYul/contracts/dirty_memory_int32.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_memory_int32.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_dirty_memory_static_array(harness):
    """viaYul/contracts/dirty_memory_static_array.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_memory_static_array.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_dirty_memory_struct(harness):
    """viaYul/contracts/dirty_memory_struct.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_memory_struct.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_dirty_memory_uint32(harness):
    """viaYul/contracts/dirty_memory_uint32.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/dirty_memory_uint32.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_empty_return_corrupted_free_memory_pointer(harness):
    """viaYul/contracts/empty_return_corrupted_free_memory_pointer.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/empty_return_corrupted_free_memory_pointer.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_exp(harness):
    """viaYul/contracts/exp.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp.sol")
    # f(uint256,uint256): 0, 0 -> 1
    r = harness.call(app, "f(uint256,uint256)", 0, 0)
    assert r.abi_return == 1
    # f(uint256,uint256): 0, 1 -> 0x00
    r = harness.call(app, "f(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # f(uint256,uint256): 0, 2 -> 0x00
    r = harness.call(app, "f(uint256,uint256)", 0, 2)
    assert r.abi_return == 0
    # f(uint256,uint256): 1, 0 -> 1
    r = harness.call(app, "f(uint256,uint256)", 1, 0)
    assert r.abi_return == 1
    # f(uint256,uint256): 1, 1 -> 1
    r = harness.call(app, "f(uint256,uint256)", 1, 1)
    assert r.abi_return == 1
    # f(uint256,uint256): 1, 2 -> 1
    r = harness.call(app, "f(uint256,uint256)", 1, 2)
    assert r.abi_return == 1
    # f(uint256,uint256): 2, 0 -> 1
    r = harness.call(app, "f(uint256,uint256)", 2, 0)
    assert r.abi_return == 1
    # f(uint256,uint256): 2, 1 -> 2
    r = harness.call(app, "f(uint256,uint256)", 2, 1)
    assert r.abi_return == 2
    # f(uint256,uint256): 2, 2 -> 4
    r = harness.call(app, "f(uint256,uint256)", 2, 2)
    assert r.abi_return == 4
    # f(uint256,uint256): 7, 63 -> 174251498233690814305510551794710260107945042018748343
    r = harness.call(app, "f(uint256,uint256)", 7, 63)
    assert r.abi_return == 174251498233690814305510551794710260107945042018748343
    # f(uint256,uint256): 128, 2 -> 0x4000
    r = harness.call(app, "f(uint256,uint256)", 128, 2)
    assert r.abi_return == 16384

def test_exp_literals(harness):
    """viaYul/contracts/exp_literals.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_literals.sol", via_yul_behavior=True)
    # exp_2(uint256): 255 -> 57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_2(uint256)", 255)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_2(uint256): 256 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_2(uint256)", 256, expect_revert=True)
    assert r.reverted
    # exp_minus_2(uint256): 255 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_minus_2(uint256)", 255)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_minus_2(uint256): 256 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_minus_2(uint256)", 256, expect_revert=True)
    assert r.reverted
    # exp_uint_max(uint256): 1 -> 115792089237316195423570985008687907853269984665640564039457584007913129639935
    r = harness.call(app, "exp_uint_max(uint256)", 1)
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # exp_uint_max(uint256): 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_uint_max(uint256)", 2, expect_revert=True)
    assert r.reverted
    # exp_int_max(uint256): 1 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_int_max(uint256)", 1)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_int_max(uint256): 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_int_max(uint256)", 2, expect_revert=True)
    assert r.reverted
    # exp_5(uint256): 110 -> 77037197775489434122239117703397092741524065928615527809597551822662353515625
    r = harness.call(app, "exp_5(uint256)", 110)
    assert r.abi_return == 77037197775489434122239117703397092741524065928615527809597551822662353515625
    # exp_5(uint256): 111 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_5(uint256)", 111, expect_revert=True)
    assert r.reverted
    # exp_minus_5(uint256): 109 -> -15407439555097886824447823540679418548304813185723105561919510364532470703125
    r = harness.call(app, "exp_minus_5(uint256)", 109)
    assert r.abi_return == -15407439555097886824447823540679418548304813185723105561919510364532470703125
    # exp_minus_5(uint256): 110 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_minus_5(uint256)", 110, expect_revert=True)
    assert r.reverted
    # exp_256(uint256): 31 -> 452312848583266388373324160190187140051835877600158453279131187530910662656
    r = harness.call(app, "exp_256(uint256)", 31)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662656
    # exp_256(uint256): 32 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_256(uint256)", 32, expect_revert=True)
    assert r.reverted
    # exp_minus_256(uint256): 31 -> -452312848583266388373324160190187140051835877600158453279131187530910662656
    r = harness.call(app, "exp_minus_256(uint256)", 31)
    assert r.abi_return == -452312848583266388373324160190187140051835877600158453279131187530910662656
    # exp_minus_256(uint256): 32 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "exp_minus_256(uint256)", 32, expect_revert=True)
    assert r.reverted

def test_exp_literals_success(harness):
    """viaYul/contracts/exp_literals_success.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_literals_success.sol")
    # exp_2(uint256): 255 -> 57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_2(uint256)", 255)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_minus_2(uint256): 255 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_minus_2(uint256)", 255)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_uint_max(uint256): 1 -> 115792089237316195423570985008687907853269984665640564039457584007913129639935
    r = harness.call(app, "exp_uint_max(uint256)", 1)
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # exp_int_max(uint256): 1 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "exp_int_max(uint256)", 1)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # exp_5(uint256): 110 -> 77037197775489434122239117703397092741524065928615527809597551822662353515625
    r = harness.call(app, "exp_5(uint256)", 110)
    assert r.abi_return == 77037197775489434122239117703397092741524065928615527809597551822662353515625
    # exp_minus_5(uint256): 109 -> -15407439555097886824447823540679418548304813185723105561919510364532470703125
    r = harness.call(app, "exp_minus_5(uint256)", 109)
    assert r.abi_return == -15407439555097886824447823540679418548304813185723105561919510364532470703125
    # exp_256(uint256): 31 -> 452312848583266388373324160190187140051835877600158453279131187530910662656
    r = harness.call(app, "exp_256(uint256)", 31)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662656
    # exp_minus_256(uint256): 31 -> -452312848583266388373324160190187140051835877600158453279131187530910662656
    r = harness.call(app, "exp_minus_256(uint256)", 31)
    assert r.abi_return == -452312848583266388373324160190187140051835877600158453279131187530910662656

def test_exp_neg(harness):
    """viaYul/contracts/exp_neg.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_neg.sol")
    # f(int256,uint256): 0, 0 -> 1
    r = harness.call(app, "f(int256,uint256)", 0, 0)
    assert r.abi_return == 1
    # f(int256,uint256): 0, 1 -> 0x00
    r = harness.call(app, "f(int256,uint256)", 0, 1)
    assert r.abi_return == 0
    # f(int256,uint256): 0, 2 -> 0x00
    r = harness.call(app, "f(int256,uint256)", 0, 2)
    assert r.abi_return == 0
    # f(int256,uint256): 1, 0 -> 1
    r = harness.call(app, "f(int256,uint256)", 1, 0)
    assert r.abi_return == 1
    # f(int256,uint256): 1, 1 -> 1
    r = harness.call(app, "f(int256,uint256)", 1, 1)
    assert r.abi_return == 1
    # f(int256,uint256): 1, 2 -> 1
    r = harness.call(app, "f(int256,uint256)", 1, 2)
    assert r.abi_return == 1
    # f(int256,uint256): 2, 0 -> 1
    r = harness.call(app, "f(int256,uint256)", 2, 0)
    assert r.abi_return == 1
    # f(int256,uint256): 2, 1 -> 2
    r = harness.call(app, "f(int256,uint256)", 2, 1)
    assert r.abi_return == 2
    # f(int256,uint256): 2, 2 -> 4
    r = harness.call(app, "f(int256,uint256)", 2, 2)
    assert r.abi_return == 4
    # f(int256,uint256): 7, 63 -> 174251498233690814305510551794710260107945042018748343
    r = harness.call(app, "f(int256,uint256)", 7, 63)
    assert r.abi_return == 174251498233690814305510551794710260107945042018748343
    # f(int256,uint256): 128, 2 -> 0x4000
    r = harness.call(app, "f(int256,uint256)", 128, 2)
    assert r.abi_return == 16384
    # f(int256,uint256): -1, 0 -> 1
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0)
    assert r.abi_return == 1
    # f(int256,uint256): -1, 1 -> -1
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 1)
    assert r.abi_return == -1
    # f(int256,uint256): -1, 2 -> 1
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 2)
    assert r.abi_return == 1
    # f(int256,uint256): -2, 0 -> 1
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0)
    assert r.abi_return == 1
    # f(int256,uint256): -2, 1 -> -2
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 1)
    assert r.abi_return == -2
    # f(int256,uint256): -2, 2 -> 4
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 2)
    assert r.abi_return == 4
    # f(int256,uint256): -7, 63 -> -174251498233690814305510551794710260107945042018748343
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 63)
    assert r.abi_return == -174251498233690814305510551794710260107945042018748343
    # f(int256,uint256): -128, 2 -> 0x4000
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 2)
    assert r.abi_return == 16384
    # f(int256,uint256): -1, 115792089237316195423570985008687907853269984665640564039457584007913129639935 -> -1
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -1
    # f(int256,uint256): -2, 255 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 255)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,uint256): -8, 85 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8, 85)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,uint256): -131072, 15 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "f(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe0000, 15)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,uint256): -32, 51 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "f(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe0, 51)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968
    # f(int256,uint256): -57896044618658097711785492504343953926634992332820282019728792003956564819968, 1 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "f(int256,uint256)", 0x8000000000000000000000000000000000000000000000000000000000000000, 1)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968

def test_exp_neg_overflow(harness):
    """viaYul/contracts/exp_neg_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_neg_overflow.sol")
    # f(int8,uint256): 2, 6 -> 64
    r = harness.call(app, "f(int8,uint256)", 2, 6)
    assert r.abi_return == 64
    # f(int8,uint256): 2, 7 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 2, 7, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 2, 8 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 2, 8, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -2, 6 -> 64
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 6)
    assert r.abi_return == 64
    # f(int8,uint256): -2, 7 -> -128
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 7)
    assert r.abi_return == -128
    # f(int8,uint256): -2, 8 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 8, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 6, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 6, 3, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 7, 2 -> 0x31
    r = harness.call(app, "f(int8,uint256)", 7, 2)
    assert r.abi_return == 49
    # f(int8,uint256): 7, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 7, 3, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -7, 2 -> 0x31
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 2)
    assert r.abi_return == 49
    # f(int8,uint256): -7, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 3, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -7, 4 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 4, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 127, 31 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 127, 31, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 127, 131 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 127, 131, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -128, 0 -> 1
    r = harness.call(app, "f(int8,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 0)
    assert r.abi_return == 1
    # f(int8,uint256): -128, 1 -> -128
    r = harness.call(app, "f(int8,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 1)
    assert r.abi_return == -128
    # f(int8,uint256): -128, 31 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 31, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -128, 131 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, 131, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -11, 2 -> 121
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff5, 2)
    assert r.abi_return == 121
    # f(int8,uint256): -12, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff4, 2, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): 12, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 12, 2, expect_revert=True)
    assert r.reverted
    # f(int8,uint256): -5, 3 -> -125
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb, 3)
    assert r.abi_return == -125
    # f(int8,uint256): -6, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(int8,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa, 3, expect_revert=True)
    assert r.reverted
    # g(int256,uint256): -7, 90 -> 11450477594321044359340126713545146077054004823284978858214566372120240027249
    r = harness.call(app, "g(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 90)
    assert r.abi_return == 11450477594321044359340126713545146077054004823284978858214566372120240027249
    # g(int256,uint256): -7, 91 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int256,uint256)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9, 91, expect_revert=True)
    assert r.reverted
    # g(int256,uint256): -63, 42 -> 3735107253208426854890677539053540390278853997836851167913009474475553834369
    r = harness.call(app, "g(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc1, 42)
    assert r.abi_return == 3735107253208426854890677539053540390278853997836851167913009474475553834369
    # g(int256,uint256): -63, 43 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(int256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc1, 43, expect_revert=True)
    assert r.reverted

def test_exp_overflow(harness):
    """viaYul/contracts/exp_overflow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_overflow.sol")
    # f(uint8,uint8): 2, 7 -> 0x80
    r = harness.call(app, "f(uint8,uint8)", 2, 7)
    assert r.abi_return == 128
    # f(uint8,uint8): 2, 8 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint8,uint8)", 2, 8, expect_revert=True)
    assert r.reverted
    # f(uint8,uint8): 15, 2 -> 225
    r = harness.call(app, "f(uint8,uint8)", 15, 2)
    assert r.abi_return == 225
    # f(uint8,uint8): 6, 3 -> 0xd8
    r = harness.call(app, "f(uint8,uint8)", 6, 3)
    assert r.abi_return == 216
    # f(uint8,uint8): 7, 2 -> 0x31
    r = harness.call(app, "f(uint8,uint8)", 7, 2)
    assert r.abi_return == 49
    # f(uint8,uint8): 7, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint8,uint8)", 7, 3, expect_revert=True)
    assert r.reverted
    # f(uint8,uint8): 7, 4 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint8,uint8)", 7, 4, expect_revert=True)
    assert r.reverted
    # f(uint8,uint8): 255, 31 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint8,uint8)", 255, 31, expect_revert=True)
    assert r.reverted
    # f(uint8,uint8): 255, 131 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f(uint8,uint8)", 255, 131, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 0x200000000000000000000000000000000, 1 -> 0x0200000000000000000000000000000000
    r = harness.call(app, "g(uint256,uint256)", 0x200000000000000000000000000000000, 1)
    assert r.abi_return == 680564733841876926926749214863536422912
    # g(uint256,uint256): 0x100000000000000000000000000000010, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 0x100000000000000000000000000000010, 2, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 0x200000000000000000000000000000000, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 0x200000000000000000000000000000000, 2, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 0x200000000000000000000000000000000, 3 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 0x200000000000000000000000000000000, 3, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 255, 31 -> 400631961586894742455537928461950192806830589109049416147172451019287109375
    r = harness.call(app, "g(uint256,uint256)", 255, 31)
    assert r.abi_return == 400631961586894742455537928461950192806830589109049416147172451019287109375
    # g(uint256,uint256): 255, 32 -> -13630939032658036097408813250890608687528184442832962921928608997994916749311
    r = harness.call(app, "g(uint256,uint256)", 255, 32)
    assert r.abi_return == -13630939032658036097408813250890608687528184442832962921928608997994916749311
    # g(uint256,uint256): 255, 33 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 255, 33, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 255, 131 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 255, 131, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 258, 31 -> 575719427506838823084316385994930914701079543089399988096291424922125729792
    r = harness.call(app, "g(uint256,uint256)", 258, 31)
    assert r.abi_return == 575719427506838823084316385994930914701079543089399988096291424922125729792
    # g(uint256,uint256): 258, 37 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 258, 37, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 258, 131 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "g(uint256,uint256)", 258, 131, expect_revert=True)
    assert r.reverted

def test_exp_various(harness):
    """viaYul/contracts/exp_various.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/exp_various.sol")
    # f(uint8,uint8): 0, 0 -> 1
    r = harness.call(app, "f(uint8,uint8)", 0, 0)
    assert r.abi_return == 1
    # f(uint8,uint8): 0, 1 -> 0x00
    r = harness.call(app, "f(uint8,uint8)", 0, 1)
    assert r.abi_return == 0
    # f(uint8,uint8): 0, 2 -> 0x00
    r = harness.call(app, "f(uint8,uint8)", 0, 2)
    assert r.abi_return == 0
    # f(uint8,uint8): 0, 3 -> 0x00
    r = harness.call(app, "f(uint8,uint8)", 0, 3)
    assert r.abi_return == 0
    # f(uint8,uint8): 1, 0 -> 1
    r = harness.call(app, "f(uint8,uint8)", 1, 0)
    assert r.abi_return == 1
    # f(uint8,uint8): 1, 1 -> 1
    r = harness.call(app, "f(uint8,uint8)", 1, 1)
    assert r.abi_return == 1
    # f(uint8,uint8): 1, 2 -> 1
    r = harness.call(app, "f(uint8,uint8)", 1, 2)
    assert r.abi_return == 1
    # f(uint8,uint8): 1, 3 -> 1
    r = harness.call(app, "f(uint8,uint8)", 1, 3)
    assert r.abi_return == 1
    # f(uint8,uint8): 2, 0 -> 1
    r = harness.call(app, "f(uint8,uint8)", 2, 0)
    assert r.abi_return == 1
    # f(uint8,uint8): 2, 1 -> 2
    r = harness.call(app, "f(uint8,uint8)", 2, 1)
    assert r.abi_return == 2
    # f(uint8,uint8): 2, 2 -> 4
    r = harness.call(app, "f(uint8,uint8)", 2, 2)
    assert r.abi_return == 4
    # f(uint8,uint8): 2, 3 -> 8
    r = harness.call(app, "f(uint8,uint8)", 2, 3)
    assert r.abi_return == 8
    # f(uint8,uint8): 3, 0 -> 1
    r = harness.call(app, "f(uint8,uint8)", 3, 0)
    assert r.abi_return == 1
    # f(uint8,uint8): 3, 1 -> 3
    r = harness.call(app, "f(uint8,uint8)", 3, 1)
    assert r.abi_return == 3
    # f(uint8,uint8): 3, 2 -> 9
    r = harness.call(app, "f(uint8,uint8)", 3, 2)
    assert r.abi_return == 9
    # f(uint8,uint8): 3, 3 -> 0x1b
    r = harness.call(app, "f(uint8,uint8)", 3, 3)
    assert r.abi_return == 27
    # f(uint8,uint8): 10, 0 -> 1
    r = harness.call(app, "f(uint8,uint8)", 10, 0)
    assert r.abi_return == 1
    # f(uint8,uint8): 10, 1 -> 0x0a
    r = harness.call(app, "f(uint8,uint8)", 10, 1)
    assert r.abi_return == 10
    # f(uint8,uint8): 10, 2 -> 100
    r = harness.call(app, "f(uint8,uint8)", 10, 2)
    assert r.abi_return == 100
    # g(uint256,uint256): 0, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 0, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 0, 1 -> 0x00
    r = harness.call(app, "g(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # g(uint256,uint256): 0, 2 -> 0x00
    r = harness.call(app, "g(uint256,uint256)", 0, 2)
    assert r.abi_return == 0
    # g(uint256,uint256): 0, 3 -> 0x00
    r = harness.call(app, "g(uint256,uint256)", 0, 3)
    assert r.abi_return == 0
    # g(uint256,uint256): 1, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 1, 1 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 1)
    assert r.abi_return == 1
    # g(uint256,uint256): 1, 2 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 2)
    assert r.abi_return == 1
    # g(uint256,uint256): 1, 3 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 3)
    assert r.abi_return == 1
    # g(uint256,uint256): 2, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 2, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 2, 1 -> 2
    r = harness.call(app, "g(uint256,uint256)", 2, 1)
    assert r.abi_return == 2
    # g(uint256,uint256): 2, 2 -> 4
    r = harness.call(app, "g(uint256,uint256)", 2, 2)
    assert r.abi_return == 4
    # g(uint256,uint256): 2, 3 -> 8
    r = harness.call(app, "g(uint256,uint256)", 2, 3)
    assert r.abi_return == 8
    # g(uint256,uint256): 3, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 3, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 3, 1 -> 3
    r = harness.call(app, "g(uint256,uint256)", 3, 1)
    assert r.abi_return == 3
    # g(uint256,uint256): 3, 2 -> 9
    r = harness.call(app, "g(uint256,uint256)", 3, 2)
    assert r.abi_return == 9
    # g(uint256,uint256): 3, 3 -> 0x1b
    r = harness.call(app, "g(uint256,uint256)", 3, 3)
    assert r.abi_return == 27
    # g(uint256,uint256): 10, 10 -> 10000000000
    r = harness.call(app, "g(uint256,uint256)", 10, 10)
    assert r.abi_return == 10000000000
    # g(uint256,uint256): 10, 77 -> -15792089237316195423570985008687907853269984665640564039457584007913129639936
    r = harness.call(app, "g(uint256,uint256)", 10, 77)
    assert r.abi_return == -15792089237316195423570985008687907853269984665640564039457584007913129639936
    # g(uint256,uint256): 256, 2 -> 0x010000
    r = harness.call(app, "g(uint256,uint256)", 256, 2)
    assert r.abi_return == 65536
    # g(uint256,uint256): 256, 31 -> 0x0100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g(uint256,uint256)", 256, 31)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662656

def test_function_address(harness):
    """viaYul/contracts/function_address.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/function_address.sol")
    # f() -> 0x1234
    r = harness.call(app, "f()")
    assert r.abi_return == 4660
    # g() -> true, true
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (True, True)
    # h(function): left(0x000000000000123442424242) -> 0x1234
    r = harness.call(app, "h(function)", 0x1234424242420000000000000000000000000000000000000000)
    assert r.abi_return == 4660

def test_function_entry_checks(harness):
    """viaYul/contracts/function_entry_checks.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/function_entry_checks.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g(uint256,uint256): 1, -2 -> 0
    r = harness.call(app, "g(uint256,uint256)", 1, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe)
    assert r.abi_return == 0
    # h(), 1 ether -> 0
    r = harness.call(app, "h()", payment_wei=1000000000000000000)
    assert r.abi_return == 0
    # i(bytes32), 1 ether: 2 -> FAILURE
    r = harness.call(app, "i(bytes32)", 2, payment_wei=1000000000000000000, expect_revert=True)
    assert r.reverted
    # i(bytes32): 2 -> 0
    r = harness.call(app, "i(bytes32)", 2)
    assert r.abi_return == 0
    # j(bool): true -> false
    r = harness.call(app, "j(bool)", True)
    assert r.abi_return is False
    # k(bytes32): 0x31 -> 0x00
    r = harness.call(app, "k(bytes32)", 49)
    assert r.abi_return == 0
    # s(): hex"4200ef" -> 0x20, 0
    r = harness.call(app, "s()", bytes.fromhex('4200ef'))
    assert tuple(r.abi_return) == (32, 0)
    # t(uint256) -> FAILURE
    r = harness.call(app, "t(uint256)", expect_revert=True)
    assert r.reverted

def test_function_pointers(harness):
    """viaYul/contracts/function_pointers.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/function_pointers.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h2() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "h2()", expect_revert=True)
    assert r.reverted
    # k2() -> FAILURE
    r = harness.call(app, "k2()", expect_revert=True)
    assert r.reverted

def test_function_selector(harness):
    """viaYul/contracts/function_selector.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/function_selector.sol")
    # f() -> left(0x26121ff0)
    r = harness.call(app, "f()")
    # TODO: verify expected: left(0x26121ff0)
    assert not r.reverted
    # h(function): left(0x000000000000000042424242) -> left(0x42424242)
    r = harness.call(app, "h(function)", 0x424242420000000000000000000000000000000000000000)
    # TODO: verify expected: left(0x42424242)
    assert not r.reverted

def test_if_(harness):
    """viaYul/contracts/if.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/if.sol")
    # f(bool): 0 -> 23
    r = harness.call(app, "f(bool)", 0)
    assert r.abi_return == 23
    # f(bool): 1 -> 42
    r = harness.call(app, "f(bool)", 1)
    assert r.abi_return == 42
    # g(bool): 0 -> 23
    r = harness.call(app, "g(bool)", 0)
    assert r.abi_return == 23
    # g(bool): 1 -> 42
    r = harness.call(app, "g(bool)", 1)
    assert r.abi_return == 42
    # h(bool): 0 -> 23
    r = harness.call(app, "h(bool)", 0)
    assert r.abi_return == 23
    # h(bool): 1 -> 42
    r = harness.call(app, "h(bool)", 1)
    assert r.abi_return == 42
    # i(bool): 0 -> 23
    r = harness.call(app, "i(bool)", 0)
    assert r.abi_return == 23
    # i(bool): 1 -> 42
    r = harness.call(app, "i(bool)", 1)
    assert r.abi_return == 42
    # j(uint256,uint256): 1, 3 -> 1, 100
    r = harness.call(app, "j(uint256,uint256)", 1, 3)
    assert tuple(r.abi_return) == (1, 100)
    # j(uint256,uint256): 3, 1 -> 3, 100
    r = harness.call(app, "j(uint256,uint256)", 3, 1)
    assert tuple(r.abi_return) == (3, 100)
    # j(uint256,uint256): 10, 23 -> 23, 100
    r = harness.call(app, "j(uint256,uint256)", 10, 23)
    assert tuple(r.abi_return) == (23, 100)
    # j(uint256,uint256): 23, 10 -> 10, 100
    r = harness.call(app, "j(uint256,uint256)", 23, 10)
    assert tuple(r.abi_return) == (10, 100)
    # k(uint256,uint256): 1, 3 -> 1, 100
    r = harness.call(app, "k(uint256,uint256)", 1, 3)
    assert tuple(r.abi_return) == (1, 100)
    # k(uint256,uint256): 3, 1 -> 3, 100
    r = harness.call(app, "k(uint256,uint256)", 3, 1)
    assert tuple(r.abi_return) == (3, 100)
    # k(uint256,uint256): 3, 3 -> 99, 99
    r = harness.call(app, "k(uint256,uint256)", 3, 3)
    assert tuple(r.abi_return) == (99, 99)
    # k(uint256,uint256): 10, 23 -> 23, 17
    r = harness.call(app, "k(uint256,uint256)", 10, 23)
    assert tuple(r.abi_return) == (23, 17)
    # k(uint256,uint256): 23, 10 -> 10, 17
    r = harness.call(app, "k(uint256,uint256)", 23, 10)
    assert tuple(r.abi_return) == (10, 17)
    # k(uint256,uint256): 23, 23 -> 23, 13
    r = harness.call(app, "k(uint256,uint256)", 23, 23)
    assert tuple(r.abi_return) == (23, 13)

def test_keccak(harness):
    """viaYul/contracts/keccak.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/keccak.sol")
    # keccak1() -> 0x64e604787cbf194841e7b68d7cd28786f6c9a0a3ab9f8b0a0e87cb4387ab0107
    r = harness.call(app, "keccak1()")
    assert r.abi_return == 45637690538541992090000098772847886457082422231295691457910964509567538102535
    # keccak2() -> 0x64e604787cbf194841e7b68d7cd28786f6c9a0a3ab9f8b0a0e87cb4387ab0107
    r = harness.call(app, "keccak2()")
    assert r.abi_return == 45637690538541992090000098772847886457082422231295691457910964509567538102535

def test_local_address_assignment(harness):
    """viaYul/contracts/local_address_assignment.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/local_address_assignment.sol")
    # f(address): 0x1234 -> 0x1234
    r = harness.call(app, "f(address)", 4660)
    assert r.abi_return == 4660

def test_local_assignment(harness):
    """viaYul/contracts/local_assignment.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/local_assignment.sol")
    # f(uint256): 6 -> 6
    r = harness.call(app, "f(uint256)", 6)
    assert r.abi_return == 6

def test_local_bool_assignment(harness):
    """viaYul/contracts/local_bool_assignment.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/local_bool_assignment.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True

def test_local_tuple_assignment(harness):
    """viaYul/contracts/local_tuple_assignment.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/local_tuple_assignment.sol")
    # x() -> 17
    r = harness.call(app, "x()")
    assert r.abi_return == 17
    # f(uint256,uint256): 23, 42 -> 23, 42
    r = harness.call(app, "f(uint256,uint256)", 23, 42)
    assert tuple(r.abi_return) == (23, 42)
    # x() -> 42
    r = harness.call(app, "x()")
    assert r.abi_return == 42
    # g() -> 3, 42, 1
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (3, 42, 1)
    # h() -> 4, 42, 1
    r = harness.call(app, "h()")
    assert tuple(r.abi_return) == (4, 42, 1)
    # i() -> 42, 23, 17, 13
    r = harness.call(app, "i()")
    assert tuple(r.abi_return) == (42, 23, 17, 13)

def test_local_variable_without_init(harness):
    """viaYul/contracts/local_variable_without_init.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/local_variable_without_init.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_mapping_enum_key_getter(harness):
    """viaYul/contracts/mapping_enum_key_getter.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/mapping_enum_key_getter.sol")
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert r.abi_return == 0
    # table(uint8): 0x01 -> 0
    r = harness.call(app, "table(uint8)", 1)
    assert r.abi_return == 0
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert r.abi_return == 0
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert r.abi_return == 161
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert r.abi_return == 239
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert r.abi_return == 161
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert r.abi_return == 239
    # table(uint8): 0x01 -> 0x05
    r = harness.call(app, "table(uint8)", 1)
    assert r.abi_return == 5
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_getters(harness):
    """viaYul/contracts/mapping_getters.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/mapping_getters.sol")
    # m1(uint256): 0 -> 0
    r = harness.call(app, "m1(uint256)", 0)
    assert r.abi_return == 0
    # m1(uint256): 0x01 -> 0
    r = harness.call(app, "m1(uint256)", 1)
    assert r.abi_return == 0
    # m1(uint256): 0xa7 -> 0
    r = harness.call(app, "m1(uint256)", 167)
    assert r.abi_return == 0
    # set(uint256,uint256): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint256,uint256)", 1, 161)
    # (void return — call succeeding is the assertion)
    # m1(uint256): 0 -> 0
    r = harness.call(app, "m1(uint256)", 0)
    assert r.abi_return == 0
    # m1(uint256): 0x01 -> 0xa1
    r = harness.call(app, "m1(uint256)", 1)
    assert r.abi_return == 161
    # m1(uint256): 0xa7 -> 0
    r = harness.call(app, "m1(uint256)", 167)
    assert r.abi_return == 0
    # set(uint256,uint256): 0x00, 0xef ->
    r = harness.call(app, "set(uint256,uint256)", 0, 239)
    # (void return — call succeeding is the assertion)
    # m1(uint256): 0 -> 0xef
    r = harness.call(app, "m1(uint256)", 0)
    assert r.abi_return == 239
    # m1(uint256): 0x01 -> 0xa1
    r = harness.call(app, "m1(uint256)", 1)
    assert r.abi_return == 161
    # m1(uint256): 0xa7 -> 0
    r = harness.call(app, "m1(uint256)", 167)
    assert r.abi_return == 0
    # set(uint256,uint256): 0x01, 0x05 ->
    r = harness.call(app, "set(uint256,uint256)", 1, 5)
    # (void return — call succeeding is the assertion)
    # m1(uint256): 0 -> 0xef
    r = harness.call(app, "m1(uint256)", 0)
    assert r.abi_return == 239
    # m1(uint256): 0x01 -> 0x05
    r = harness.call(app, "m1(uint256)", 1)
    assert r.abi_return == 5
    # m1(uint256): 0xa7 -> 0
    r = harness.call(app, "m1(uint256)", 167)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 0, 0)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0, 0x01 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0xa7, 0 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 167, 0)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0xa7, 0x01 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 167, 1)
    assert r.abi_return == 0
    # set(uint256,uint256,uint256): 0xa7, 0x01, 0x23
    r = harness.call(app, "set(uint256,uint256,uint256)", 167, 1, 35)
    # (void return — call succeeding is the assertion)
    # m2(uint256,uint256): 0, 0x01 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0xa7, 0 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 167, 0)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0xa7, 0x01 -> 0x23
    r = harness.call(app, "m2(uint256,uint256)", 167, 1)
    assert r.abi_return == 35
    # set(uint256,uint256,uint256): 0, 0x01, 0xef
    r = harness.call(app, "set(uint256,uint256,uint256)", 0, 1, 239)
    # (void return — call succeeding is the assertion)
    # m2(uint256,uint256): 0, 0x01 -> 0xef
    r = harness.call(app, "m2(uint256,uint256)", 0, 1)
    assert r.abi_return == 239
    # m2(uint256,uint256): 0xa7, 0 -> 0
    r = harness.call(app, "m2(uint256,uint256)", 167, 0)
    assert r.abi_return == 0
    # m2(uint256,uint256): 0xa7, 0x01 -> 0x23
    r = harness.call(app, "m2(uint256,uint256)", 167, 1)
    assert r.abi_return == 35

def test_mapping_string_key(harness):
    """viaYul/contracts/mapping_string_key.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/mapping_string_key.sol")
    # set(string): 0x20, 32, "01234567890123456789012345678901" ->
    r = harness.call(app, "set(string)", 32, 32, bytes.fromhex('3031323334353637383930313233343536373839303132333435363738393031'))
    # (void return — call succeeding is the assertion)

def test_memory_struct_allow(harness):
    """viaYul/contracts/memory_struct_allow.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/memory_struct_allow.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (0, 0)

def test_msg_sender(harness):
    """viaYul/contracts/msg_sender.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/msg_sender.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_negation_bug(harness):
    """viaYul/contracts/negation_bug.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/negation_bug.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_require(harness):
    """viaYul/contracts/require.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/require.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True
    # f(bool): false -> FAILURE
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # fail() -> FAILURE
    r = harness.call(app, "fail()", expect_revert=True)
    assert r.reverted
    # succeed() -> true
    r = harness.call(app, "succeed()")
    assert r.abi_return is True
    # f2(bool): true -> true
    r = harness.call(app, "f2(bool)", True)
    assert r.abi_return is True
    # f2(bool): false -> FAILURE, hex"08c379a0", 0x20, 14, "fancy message!"
    r = harness.call(app, "f2(bool)", False, expect_revert=True)
    assert r.reverted
    # f3(bool): true -> true
    r = harness.call(app, "f3(bool)", True)
    assert r.abi_return is True
    # f3(bool): false -> FAILURE, hex"08c379a0", 0x20, 3, "msg"
    r = harness.call(app, "f3(bool)", False, expect_revert=True)
    assert r.reverted
    # f4(bool): true -> true
    r = harness.call(app, "f4(bool)", True)
    assert r.abi_return is True
    # f4(bool): false -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "f4(bool)", False, expect_revert=True)
    assert r.reverted

def test_return_(harness):
    """viaYul/contracts/return.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/return.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7

def test_return_and_convert(harness):
    """viaYul/contracts/return_and_convert.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/return_and_convert.sol")
    # f() -> 255
    r = harness.call(app, "f()")
    assert r.abi_return == 255

def test_return_storage_pointers(harness):
    """viaYul/contracts/return_storage_pointers.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/return_storage_pointers.sol")
    # g() -> 0, 0
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (0, 0)

def test_short_circuit(harness):
    """viaYul/contracts/short_circuit.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/short_circuit.sol")
    # or(uint256): 0 -> true, 0
    r = harness.call(app, "or(uint256)", 0)
    # TODO: verify expected: true | 0
    assert not r.reverted
    # and(uint256): 0 -> true, 8
    r = harness.call(app, "and(uint256)", 0)
    # TODO: verify expected: true | 8
    assert not r.reverted
    # or(uint256): 1 -> true, 8
    r = harness.call(app, "or(uint256)", 1)
    # TODO: verify expected: true | 8
    assert not r.reverted
    # and(uint256): 1 -> false, 1
    r = harness.call(app, "and(uint256)", 1)
    # TODO: verify expected: false | 1
    assert not r.reverted

def test_simple_assignment(harness):
    """viaYul/contracts/simple_assignment.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/simple_assignment.sol")
    # f(uint256,uint256): 5, 6 -> 5, 6
    r = harness.call(app, "f(uint256,uint256)", 5, 6)
    assert tuple(r.abi_return) == (5, 6)

def test_simple_inline_asm(harness):
    """viaYul/contracts/simple_inline_asm.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/simple_inline_asm.sol")
    # f() -> 6
    r = harness.call(app, "f()")
    assert r.abi_return == 6

def test_smoke_test(harness):
    """viaYul/contracts/smoke_test.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/smoke_test.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_string_format(harness):
    """viaYul/contracts/string_format.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/string_format.sol")
    # f1() -> 0x20, 6, left(0x616263616263)
    r = harness.call(app, "f1()")
    # TODO: verify expected: 0x20 | 6 | left(0x616263616263)
    assert not r.reverted
    # f2() -> 32, 47, 44048183223289766195424279195050628400112610419087780792899004030957505095210, 18165586057823232067963737336409268114628061002662705707816940456850361417728
    r = harness.call(app, "f2()")
    assert tuple(r.abi_return) == (32, 47, 44048183223289766195424279195050628400112610419087780792899004030957505095210, 18165586057823232067963737336409268114628061002662705707816940456850361417728)
    # g() -> left(0x616263616263)
    r = harness.call(app, "g()")
    # TODO: verify expected: left(0x616263616263)
    assert not r.reverted
    # h() -> left(0xcafecafe)
    r = harness.call(app, "h()")
    # TODO: verify expected: left(0xcafecafe)
    assert not r.reverted

def test_string_literals(harness):
    """viaYul/contracts/string_literals.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/string_literals.sol")
    # short_dyn() -> 0x20, 3, "abc"
    r = harness.call(app, "short_dyn()")
    assert r.abi_return == 'abc'
    # long_dyn() -> 0x20, 80, "12345678901234567890123456789012", "34567890123456789012345678901234", "5678901234567890"
    r = harness.call(app, "long_dyn()")
    # TODO: verify expected: 0x20 | 80 | "12345678901234567890123456789012" | "34567890123456789012345678901234" | "5678901234567890"
    assert not r.reverted
    # short_bytes_dyn() -> 0x20, 3, "abc"
    r = harness.call(app, "short_bytes_dyn()")
    assert r.abi_return == 'abc'
    # long_bytes_dyn() -> 0x20, 80, "12345678901234567890123456789012", "34567890123456789012345678901234", "5678901234567890"
    r = harness.call(app, "long_bytes_dyn()")
    # TODO: verify expected: 0x20 | 80 | "12345678901234567890123456789012" | "34567890123456789012345678901234" | "5678901234567890"
    assert not r.reverted
    # bytesNN() -> 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "bytesNN()")
    assert r.abi_return == 44048180597813453602326562734351324025098966208897425494240603688123167145984
    # bytesNN_padded() -> 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "bytesNN_padded()")
    assert r.abi_return == 44048180597813453602326562734351324025098966208897425494240603688123167145984

def test_struct_member_access(harness):
    """viaYul/contracts/struct_member_access.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/struct_member_access.sol")
    # f((uint256,uint256[],uint256)): 0x20, 42, 0x60, 21, 3, 1, 2, 3 -> 42, 0x60, 21, 3, 1, 2, 3
    r = harness.call(app, "f((uint256,uint256[],uint256))", 32, 42, 96, 21, 3, 1, 2, 3)
    # TODO: verify structural decoding matches expected: 42, 96, 21, 3, 1, 2, 3
    assert not r.reverted
    # g((uint256,uint256[],uint256)): 0x20, 42, 0x60, 21, 3, 1, 2, 3 -> 42, 3, 21, 1, 2, 3
    r = harness.call(app, "g((uint256,uint256[],uint256))", 32, 42, 96, 21, 3, 1, 2, 3)
    # TODO: verify structural decoding matches expected: 42, 3, 21, 1, 2, 3
    assert not r.reverted
    # g2((uint256,uint256[],uint256),(uint256,uint256[],uint256)): 0x40, 0x0120, 42, 0x60, 21, 2, 1, 2, 3, 7, 0x80, 9, 0, 1, 17 -> 42, 21, 7, 1, 9, 17
    r = harness.call(app, "g2((uint256,uint256[],uint256),(uint256,uint256[],uint256))", 64, 288, 42, 96, 21, 2, 1, 2, 3, 7, 128, 9, 0, 1, 17)
    # TODO: verify structural decoding matches expected: 42, 21, 7, 1, 9, 17
    assert not r.reverted
    # h() -> 42, 3, 21, 1, 2, 3
    r = harness.call(app, "h()")
    # TODO: verify structural decoding matches expected: 42, 3, 21, 1, 2, 3
    assert not r.reverted

def test_tuple_evaluation_order(harness):
    """viaYul/contracts/tuple_evaluation_order.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/tuple_evaluation_order.sol")
    # f() -> 3, 1
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (3, 1)

def test_unary_fixedbytes(harness):
    """viaYul/contracts/unary_fixedbytes.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/unary_fixedbytes.sol")
    # conv(bytes25): left(0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff) -> 0xff00ff00ff00ff00ff00ff00ff00ff00ffffffffffffffffffffffffffffffff
    r = harness.call(app, "conv(bytes25)", 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000)
    assert r.abi_return == 115341536360906404779899502576747487978355861310392627753169668796238104887295
    # upcast(bytes25): left(0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff) -> 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff0000000000000000
    r = harness.call(app, "upcast(bytes25)", 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000)
    assert r.abi_return == 450552876409790643671482431940419874915447411150352389258518043828168949760
    # downcast(bytes25): left(0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff) -> 0xff00ff00ff00ff00ff00ff0000000000000000000000000000000000000000
    r = harness.call(app, "downcast(bytes25)", 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000)
    assert r.abi_return == 450552876409790643671482431934733098116494481507536494967864984390105825280
    # r_b32() -> 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff
    r = harness.call(app, "r_b32()")
    assert r.abi_return == 450552876409790643671482431940419874915447411150352389258589821042463539455
    # r_b25() -> 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff0000000000000000
    r = harness.call(app, "r_b25()")
    assert r.abi_return == 450552876409790643671482431940419874915447411150352389258518043828168949760
    # r_b16() -> 0xff00ff00ff00ff00ff00ff00ff00ff00000000000000000000000000000000
    r = harness.call(app, "r_b16()")
    assert r.abi_return == 450552876409790643671482431940419874914123355247936286287915211675024752640
    # r_b8() -> 0xff00ff00ff00ff000000000000000000000000000000000000000000000000
    r = harness.call(app, "r_b8()")
    assert r.abi_return == 450552876409790643647057911569265475056164183822696099701771317367506206720
    # r_b4() -> 0xff00ff00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "r_b4()")
    assert r.abi_return == 450552876304888127456888502778291411541830171175110582166992262750231592960
    # r_b1() -> 0xaa00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "r_b1()")
    assert r.abi_return == 76893184259155286023465107232331813808812099192026937057452301880254812651520
    # a_b32() -> 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff
    r = harness.call(app, "a_b32()")
    assert r.abi_return == 450552876409790643671482431940419874915447411150352389258589821042463539455
    # a_b25() -> 0xff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff0000000000000000
    r = harness.call(app, "a_b25()")
    assert r.abi_return == 450552876409790643671482431940419874915447411150352389258518043828168949760
    # a_b16() -> 0xff00ff00ff00ff00ff00ff00ff00ff00000000000000000000000000000000
    r = harness.call(app, "a_b16()")
    assert r.abi_return == 450552876409790643671482431940419874914123355247936286287915211675024752640
    # a_b8() -> 0xff00ff00ff00ff000000000000000000000000000000000000000000000000
    r = harness.call(app, "a_b8()")
    assert r.abi_return == 450552876409790643647057911569265475056164183822696099701771317367506206720
    # a_b4() -> 0xff00ff00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "a_b4()")
    assert r.abi_return == 450552876304888127456888502778291411541830171175110582166992262750231592960
    # a_b1() -> 0xaa00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "a_b1()")
    assert r.abi_return == 76893184259155286023465107232331813808812099192026937057452301880254812651520

def test_unary_operations(harness):
    """viaYul/contracts/unary_operations.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/unary_operations.sol", via_yul_behavior=True)
    # preincr_s8(int8): 128 -> FAILURE
    r = harness.call(app, "preincr_s8(int8)", 128, expect_revert=True)
    assert r.reverted
    # postincr_s8(int8): 128 -> FAILURE
    r = harness.call(app, "postincr_s8(int8)", 128, expect_revert=True)
    assert r.reverted
    # preincr_s8(int8): 127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "preincr_s8(int8)", 127, expect_revert=True)
    assert r.reverted
    # postincr_s8(int8): 127 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "postincr_s8(int8)", 127, expect_revert=True)
    assert r.reverted
    # preincr_s8(int8): 126 -> 127, 127
    r = harness.call(app, "preincr_s8(int8)", 126)
    assert tuple(r.abi_return) == (127, 127)
    # postincr_s8(int8): 126 -> 126, 127
    r = harness.call(app, "postincr_s8(int8)", 126)
    assert tuple(r.abi_return) == (126, 127)
    # predecr_s8(int8): -128 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "predecr_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, expect_revert=True)
    assert r.reverted
    # postdecr_s8(int8): -128 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "postdecr_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, expect_revert=True)
    assert r.reverted
    # predecr_s8(int8): -127 -> -128, -128
    r = harness.call(app, "predecr_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81)
    assert tuple(r.abi_return) == (-128, -128)
    # postdecr_s8(int8): -127 -> -127, -128
    r = harness.call(app, "postdecr_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81)
    assert tuple(r.abi_return) == (-127, -128)
    # preincr_s8(int8): -5 -> -4, -4
    r = harness.call(app, "preincr_s8(int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert tuple(r.abi_return) == (-4, -4)
    # postincr_s8(int8): -5 -> -5, -4
    r = harness.call(app, "postincr_s8(int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert tuple(r.abi_return) == (-5, -4)
    # predecr_s8(int8): -5 -> -6, -6
    r = harness.call(app, "predecr_s8(int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert tuple(r.abi_return) == (-6, -6)
    # postdecr_s8(int8): -5 -> -5, -6
    r = harness.call(app, "postdecr_s8(int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb)
    assert tuple(r.abi_return) == (-5, -6)
    # preincr_u8(uint8): 255 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "preincr_u8(uint8)", 255, expect_revert=True)
    assert r.reverted
    # postincr_u8(uint8): 255 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "postincr_u8(uint8)", 255, expect_revert=True)
    assert r.reverted
    # preincr_u8(uint8): 254 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "preincr_u8(uint8)", 254, expect_revert=True)
    assert r.reverted
    # postincr_u8(uint8): 254 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "postincr_u8(uint8)", 254, expect_revert=True)
    assert r.reverted
    # predecr_u8(uint8): 0 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "predecr_u8(uint8)", 0, expect_revert=True)
    assert r.reverted
    # postdecr_u8(uint8): 0 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "postdecr_u8(uint8)", 0, expect_revert=True)
    assert r.reverted
    # predecr_u8(uint8): 1 -> 0
    r = harness.call(app, "predecr_u8(uint8)", 1)
    assert r.abi_return == 0
    # postdecr_u8(uint8): 1 -> 1
    r = harness.call(app, "postdecr_u8(uint8)", 1)
    assert r.abi_return == 1
    # preincr_u8(uint8): 2 -> 6
    r = harness.call(app, "preincr_u8(uint8)", 2)
    assert r.abi_return == 6
    # postincr_u8(uint8): 2 -> 5
    r = harness.call(app, "postincr_u8(uint8)", 2)
    assert r.abi_return == 5
    # predecr_u8(uint8): 2 -> 2
    r = harness.call(app, "predecr_u8(uint8)", 2)
    assert r.abi_return == 2
    # postdecr_u8(uint8): 2 -> 3
    r = harness.call(app, "postdecr_u8(uint8)", 2)
    assert r.abi_return == 3
    # preincr(uint256): 2 -> 6
    r = harness.call(app, "preincr(uint256)", 2)
    assert r.abi_return == 6
    # postincr(uint256): 2 -> 5
    r = harness.call(app, "postincr(uint256)", 2)
    assert r.abi_return == 5
    # predecr(uint256): 2 -> 2
    r = harness.call(app, "predecr(uint256)", 2)
    assert r.abi_return == 2
    # postdecr(uint256): 2 -> 3
    r = harness.call(app, "postdecr(uint256)", 2)
    assert r.abi_return == 3
    # not(bool): true -> false
    r = harness.call(app, "not(bool)", True)
    assert r.abi_return is False
    # not(bool): false -> true
    r = harness.call(app, "not(bool)", False)
    assert r.abi_return is True
    # bitnot(int256): 5 -> -6
    r = harness.call(app, "bitnot(int256)", 5)
    assert r.abi_return == -6
    # bitnot(int256): 10 -> -11
    r = harness.call(app, "bitnot(int256)", 10)
    assert r.abi_return == -11
    # bitnot(int256): 0 -> -1
    r = harness.call(app, "bitnot(int256)", 0)
    assert r.abi_return == -1
    # bitnot(int256): -100 -> 99
    r = harness.call(app, "bitnot(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9c)
    assert r.abi_return == 99
    # bitnot_u8(uint8): 100 -> 155
    r = harness.call(app, "bitnot_u8(uint8)", 100)
    assert r.abi_return == 155
    # bitnot_s8() -> 99
    r = harness.call(app, "bitnot_s8()")
    assert r.abi_return == 99
    # negate(int256): -57896044618658097711785492504343953926634992332820282019728792003956564819968 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "negate(int256)", 0x8000000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # negate(int256): -57896044618658097711785492504343953926634992332820282019728792003956564819967 -> 57896044618658097711785492504343953926634992332820282019728792003956564819967
    r = harness.call(app, "negate(int256)", 0x8000000000000000000000000000000000000000000000000000000000000001)
    assert r.abi_return == 57896044618658097711785492504343953926634992332820282019728792003956564819967
    # negate(int256): 0 -> 0
    r = harness.call(app, "negate(int256)", 0)
    assert r.abi_return == 0
    # negate(int256): 1 -> -1
    r = harness.call(app, "negate(int256)", 1)
    assert r.abi_return == -1
    # negate(int256): -1 -> 1
    r = harness.call(app, "negate(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1
    # negate_s8(int8): -128 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "negate_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff80, expect_revert=True)
    assert r.reverted
    # negate_s8(int8): -138 -> FAILURE
    r = harness.call(app, "negate_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff76, expect_revert=True)
    assert r.reverted
    # negate_s8(int8): -127 -> 127
    r = harness.call(app, "negate_s8(int8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff81)
    assert r.abi_return == 127
    # negate_s8(int8): 127 -> -127
    r = harness.call(app, "negate_s8(int8)", 127)
    assert r.abi_return == -127
    # negate_s16(int16): -32768 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "negate_s16(int16)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8000, expect_revert=True)
    assert r.reverted
    # negate_s16(int16): -32767 -> 32767
    r = harness.call(app, "negate_s16(int16)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8001)
    assert r.abi_return == 32767

def test_various_inline_asm(harness):
    """viaYul/contracts/various_inline_asm.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/various_inline_asm.sol")
    # f() -> 70
    r = harness.call(app, "f()")
    assert r.abi_return == 70

def test_virtual_functions(harness):
    """viaYul/contracts/virtual_functions.sol"""
    app = harness.compile_and_deploy("viaYul/contracts/virtual_functions.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
    # f1() -> 3
    r = harness.call(app, "f1()")
    assert r.abi_return == 3
    # f2() -> 3
    r = harness.call(app, "f2()")
    assert r.abi_return == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3
