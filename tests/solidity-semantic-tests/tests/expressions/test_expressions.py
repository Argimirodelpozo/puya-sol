"""Auto-generated tests for the expressions category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_bit_operators(harness):
    """expressions/bit_operators.sol"""
    app = harness.compile_and_deploy("expressions/bit_operators.sol")
    # f() -> 3855, 268374015, 268370160
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (3855, 268374015, 268370160)

def test_bytes_comparison(harness):
    """expressions/bytes_comparison.sol"""
    app = harness.compile_and_deploy("expressions/bytes_comparison.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_conditional_expression_different_types(harness):
    """expressions/conditional_expression_different_types.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_different_types.sol")
    # f(bool): true -> 0xcd
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return == 205
    # f(bool): false -> 0xabab
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return == 43947

def test_conditional_expression_false_literal(harness):
    """expressions/conditional_expression_false_literal.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_false_literal.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert r.abi_return == 10

def test_conditional_expression_functions(harness):
    """expressions/conditional_expression_functions.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_functions.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return == 2

def test_conditional_expression_multiple(harness):
    """expressions/conditional_expression_multiple.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_multiple.sol")
    # f(uint256): 1001 -> 1000
    r = harness.call(app, "f(uint256)", 1001)
    assert r.abi_return == 1000
    # f(uint256): 500 -> 100
    r = harness.call(app, "f(uint256)", 500)
    assert r.abi_return == 100
    # f(uint256): 80 -> 50
    r = harness.call(app, "f(uint256)", 80)
    assert r.abi_return == 50
    # f(uint256): 40 -> 10
    r = harness.call(app, "f(uint256)", 40)
    assert r.abi_return == 10

def test_conditional_expression_storage_memory_1(harness):
    """expressions/conditional_expression_storage_memory_1.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_storage_memory_1.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return == 2

def test_conditional_expression_storage_memory_2(harness):
    """expressions/conditional_expression_storage_memory_2.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_storage_memory_2.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return == 2

def test_conditional_expression_true_literal(harness):
    """expressions/conditional_expression_true_literal.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_true_literal.sol")
    # f() -> 5
    r = harness.call(app, "f()")
    assert r.abi_return == 5

def test_conditional_expression_tuples(harness):
    """expressions/conditional_expression_tuples.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_tuples.sol")
    # f(bool): true -> 1, 2
    r = harness.call(app, "f(bool)", True)
    assert tuple(r.abi_return) == (1, 2)
    # f(bool): false -> 3, 4
    r = harness.call(app, "f(bool)", False)
    assert tuple(r.abi_return) == (3, 4)

def test_conditional_expression_with_return_values(harness):
    """expressions/conditional_expression_with_return_values.sol"""
    app = harness.compile_and_deploy("expressions/conditional_expression_with_return_values.sol")
    # f(bool,uint256): true, 20 -> 20, 0
    r = harness.call(app, "f(bool,uint256)", True, 20)
    assert tuple(r.abi_return) == (20, 0)
    # f(bool,uint256): false, 20 -> 0, 20
    r = harness.call(app, "f(bool,uint256)", False, 20)
    assert tuple(r.abi_return) == (0, 20)

def test_exp_operator_const(harness):
    """expressions/exp_operator_const.sol"""
    app = harness.compile_and_deploy("expressions/exp_operator_const.sol")
    # f() -> 8
    r = harness.call(app, "f()")
    assert r.abi_return == 8

def test_exp_operator_const_signed(harness):
    """expressions/exp_operator_const_signed.sol"""
    app = harness.compile_and_deploy("expressions/exp_operator_const_signed.sol")
    # f() -> -8
    r = harness.call(app, "f()")
    assert r.abi_return == -8

def test_exp_zero_literal(harness):
    """expressions/exp_zero_literal.sol"""
    app = harness.compile_and_deploy("expressions/exp_zero_literal.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert r.abi_return == 1

def test_inc_dec_operators(harness):
    """expressions/inc_dec_operators.sol"""
    app = harness.compile_and_deploy("expressions/inc_dec_operators.sol")
    # f() -> 0x053866
    r = harness.call(app, "f()")
    assert r.abi_return == 342118

def test_module_from_ternary_expression(harness):
    """expressions/module_from_ternary_expression.sol"""
    app = harness.compile_and_deploy("expressions/module_from_ternary_expression.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_tuple_from_ternary_expression(harness):
    """expressions/tuple_from_ternary_expression.sol"""
    app = harness.compile_and_deploy("expressions/tuple_from_ternary_expression.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_unary_too_long_literal(harness):
    """expressions/unary_too_long_literal.sol"""
    app = harness.compile_and_deploy("expressions/unary_too_long_literal.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_uncalled_address_transfer_send(harness):
    """expressions/uncalled_address_transfer_send.sol"""
    app = harness.compile_and_deploy("expressions/uncalled_address_transfer_send.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
