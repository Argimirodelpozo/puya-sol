"""Auto-generated tests for the immutable category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_assign_at_declaration(harness):
    """immutable/contracts/assign_at_declaration.sol"""
    app = harness.compile_and_deploy("immutable/contracts/assign_at_declaration.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_assign_from_immutables(harness):
    """immutable/contracts/assign_from_immutables.sol"""
    app = harness.compile_and_deploy("immutable/contracts/assign_from_immutables.sol")
    # a() -> 1
    r = harness.call(app, "a()")
    assert r.abi_return == 1
    # b() -> 1
    r = harness.call(app, "b()")
    assert r.abi_return == 1
    # c() -> 1
    r = harness.call(app, "c()")
    assert r.abi_return == 1
    # d() -> 1
    r = harness.call(app, "d()")
    assert r.abi_return == 1

def test_delete(harness):
    """immutable/contracts/delete.sol"""
    app = harness.compile_and_deploy("immutable/contracts/delete.sol")
    # a() -> 0
    r = harness.call(app, "a()")
    assert r.abi_return == 0
    # b() -> 0
    r = harness.call(app, "b()")
    assert r.abi_return == 0
    # c() -> 0
    r = harness.call(app, "c()")
    assert r.abi_return == 0

def test_fun_read_in_ctor(harness):
    """immutable/contracts/fun_read_in_ctor.sol"""
    app = harness.compile_and_deploy("immutable/contracts/fun_read_in_ctor.sol")
    # readX() -> 3
    r = harness.call(app, "readX()")
    assert r.abi_return == 3
    # readA() -> 3
    r = harness.call(app, "readA()")
    assert r.abi_return == 3

def test_getter(harness):
    """immutable/contracts/getter.sol"""
    app = harness.compile_and_deploy("immutable/contracts/getter.sol")
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1

def test_getter_call_in_constructor(harness):
    """immutable/contracts/getter_call_in_constructor.sol"""
    app = harness.compile_and_deploy("immutable/contracts/getter_call_in_constructor.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_immutable_signed(harness):
    """immutable/contracts/immutable_signed.sol"""
    app = harness.compile_and_deploy("immutable/contracts/immutable_signed.sol")
    # viaasm() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "viaasm()")
    assert tuple(r.abi_return) == (115792089237316195423570985008687907853269984665640564039457584007913129639934, 44047497324925121336511606693520958599579173549109180625971642598225011015680)

def test_immutable_tag_too_large_bug(harness):
    """immutable/contracts/immutable_tag_too_large_bug.sol"""
    app = harness.compile_and_deploy("immutable/contracts/immutable_tag_too_large_bug.sol", via_yul_behavior=True)
    # f() -> -1, 1
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (-1, 1)

def test_increment_decrement(harness):
    """immutable/contracts/increment_decrement.sol"""
    app = harness.compile_and_deploy("immutable/contracts/increment_decrement.sol")
    # f() -> -1, 4
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (-1, 4)

def test_inheritance(harness):
    """immutable/contracts/inheritance.sol"""
    app = harness.compile_and_deploy("immutable/contracts/inheritance.sol")
    # f() -> 4, 3, 2, 1
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (4, 3, 2, 1)

def test_internal_function_pointer(harness):
    """immutable/contracts/internal_function_pointer.sol"""
    app = harness.compile_and_deploy("immutable/contracts/internal_function_pointer.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7
    # callZ() -> 7
    r = harness.call(app, "callZ()")
    assert r.abi_return == 7

def test_multi_creation(harness):
    """immutable/contracts/multi_creation.sol"""
    app = harness.compile_and_deploy("immutable/contracts/multi_creation.sol")
    # f() -> 3, 7, 5
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (3, 7, 5)
    # x() -> 7
    r = harness.call(app, "x()")
    assert r.abi_return == 7
    # y() -> 5
    r = harness.call(app, "y()")
    assert r.abi_return == 5

def test_multiple_initializations(harness):
    """immutable/contracts/multiple_initializations.sol"""
    app = harness.compile_and_deploy("immutable/contracts/multiple_initializations.sol")
    # get() -> 0xff
    r = harness.call(app, "get()")
    assert r.abi_return == 255

def test_read_in_ctor(harness):
    """immutable/contracts/read_in_ctor.sol"""
    app = harness.compile_and_deploy("immutable/contracts/read_in_ctor.sol")
    # readX() -> 3
    r = harness.call(app, "readX()")
    assert r.abi_return == 3

def test_small_types_in_reverse(harness):
    """immutable/contracts/small_types_in_reverse.sol"""
    app = harness.compile_and_deploy("immutable/contracts/small_types_in_reverse.sol")
    # a() -> 4660
    r = harness.call(app, "a()")
    assert r.abi_return == 4660
    # b() -> 0x0f0f
    r = harness.call(app, "b()")
    assert r.abi_return == 3855
    # c() -> 0xffff
    r = harness.call(app, "c()")
    assert r.abi_return == 65535
    # x(uint256): 0 -> 4660
    r = harness.call(app, "x(uint256)", 0)
    assert r.abi_return == 4660
    # x(uint256): 1 -> 0x0f0f
    r = harness.call(app, "x(uint256)", 1)
    assert r.abi_return == 3855
    # x(uint256): 2 -> 0xffff
    r = harness.call(app, "x(uint256)", 2)
    assert r.abi_return == 65535

def test_stub(harness):
    """immutable/contracts/stub.sol"""
    app = harness.compile_and_deploy("immutable/contracts/stub.sol")
    # f() -> 84, 23
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (84, 23)

def test_uninitialized(harness):
    """immutable/contracts/uninitialized.sol"""
    app = harness.compile_and_deploy("immutable/contracts/uninitialized.sol")
    # get() -> 0, false, 0x0
    r = harness.call(app, "get()")
    # TODO: verify expected: 0 | false | 0x0
    assert not r.reverted

def test_use_scratch(harness):
    """immutable/contracts/use_scratch.sol"""
    app = harness.compile_and_deploy("immutable/contracts/use_scratch.sol", ctor_args=[3])
    # f() -> 84, 23
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (84, 23)
    # m(uint256): 3 -> 7
    r = harness.call(app, "m(uint256)", 3)
    assert r.abi_return == 7
