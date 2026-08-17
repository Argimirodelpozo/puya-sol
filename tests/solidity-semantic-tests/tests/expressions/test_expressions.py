"""Tests for the expressions category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_bit_operators(harness):
    """expressions/contracts/bit_operators.sol"""
    app = harness.compile_and_deploy("expressions/contracts/bit_operators.sol")
    # f() -> 3855, 268374015, 268370160
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (3855, 268374015, 268370160)

def test_bytes_comparison(harness):
    """expressions/contracts/bytes_comparison.sol"""
    app = harness.compile_and_deploy("expressions/contracts/bytes_comparison.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_conditional_expression_different_types(harness):
    """expressions/contracts/conditional_expression_different_types.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_different_types.sol")
    # f(bool): true -> 0xcd
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 205
    # f(bool): false -> 0xabab
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 43947

def test_conditional_expression_false_literal(harness):
    """expressions/contracts/conditional_expression_false_literal.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_false_literal.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 10

def test_conditional_expression_functions(harness):
    """expressions/contracts/conditional_expression_functions.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_functions.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_conditional_expression_multiple(harness):
    """expressions/contracts/conditional_expression_multiple.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_multiple.sol")
    # f(uint256): 1001 -> 1000
    r = harness.call(app, "f(uint256)", 1001)
    assert as_int(r.abi_return) == 1000
    # f(uint256): 500 -> 100
    r = harness.call(app, "f(uint256)", 500)
    assert as_int(r.abi_return) == 100
    # f(uint256): 80 -> 50
    r = harness.call(app, "f(uint256)", 80)
    assert as_int(r.abi_return) == 50
    # f(uint256): 40 -> 10
    r = harness.call(app, "f(uint256)", 40)
    assert as_int(r.abi_return) == 10

def test_conditional_expression_storage_memory_1(harness):
    """expressions/contracts/conditional_expression_storage_memory_1.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_storage_memory_1.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_conditional_expression_storage_memory_2(harness):
    """expressions/contracts/conditional_expression_storage_memory_2.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_storage_memory_2.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_conditional_expression_true_literal(harness):
    """expressions/contracts/conditional_expression_true_literal.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_true_literal.sol")
    # f() -> 5
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 5

def test_conditional_expression_tuples(harness):
    """expressions/contracts/conditional_expression_tuples.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_tuples.sol")
    # f(bool): true -> 1, 2
    r = harness.call(app, "f(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # f(bool): false -> 3, 4
    r = harness.call(app, "f(bool)", False)
    assert tuple(as_int(x) for x in r.abi_return) == (3, 4)

def test_conditional_expression_with_return_values(harness):
    """expressions/contracts/conditional_expression_with_return_values.sol"""
    app = harness.compile_and_deploy("expressions/contracts/conditional_expression_with_return_values.sol")
    # f(bool,uint256): true, 20 -> 20, 0
    r = harness.call(app, "f(bool,uint256)", True, 20)
    assert tuple(as_int(x) for x in r.abi_return) == (20, 0)
    # f(bool,uint256): false, 20 -> 0, 20
    r = harness.call(app, "f(bool,uint256)", False, 20)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 20)

def test_exp_operator_const(harness):
    """expressions/contracts/exp_operator_const.sol"""
    app = harness.compile_and_deploy("expressions/contracts/exp_operator_const.sol")
    # f() -> 8
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 8

def test_exp_operator_const_signed(harness):
    """expressions/contracts/exp_operator_const_signed.sol"""
    app = harness.compile_and_deploy("expressions/contracts/exp_operator_const_signed.sol")
    # f() -> -8
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) in (-8, 115792089237316195423570985008687907853269984665640564039457584007913129639928)

def test_exp_zero_literal(harness):
    """expressions/contracts/exp_zero_literal.sol"""
    app = harness.compile_and_deploy("expressions/contracts/exp_zero_literal.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_inc_dec_operators(harness):
    """expressions/contracts/inc_dec_operators.sol"""
    app = harness.compile_and_deploy("expressions/contracts/inc_dec_operators.sol")
    # f() -> 0x053866
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 342118

@pytest.mark.xfail(reason="address(library) is a hard compile error per EvmFeaturePolicy — libraries are inlined as AVM subroutines with no deployed application identity", strict=False)
def test_module_from_ternary_expression(harness):
    """expressions/contracts/module_from_ternary_expression.sol"""
    app = harness.compile_and_deploy("expressions/contracts/module_from_ternary_expression.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_tuple_from_ternary_expression(harness):
    """expressions/contracts/tuple_from_ternary_expression.sol"""
    app = harness.compile_and_deploy("expressions/contracts/tuple_from_ternary_expression.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_unary_too_long_literal(harness):
    """expressions/contracts/unary_too_long_literal.sol"""
    app = harness.compile_and_deploy("expressions/contracts/unary_too_long_literal.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_uncalled_address_transfer_send(harness):
    """expressions/contracts/uncalled_address_transfer_send.sol"""
    app = harness.compile_and_deploy("expressions/contracts/uncalled_address_transfer_send.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
