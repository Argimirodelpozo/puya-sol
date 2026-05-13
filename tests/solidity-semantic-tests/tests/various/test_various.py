"""Tests for the various category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_address_code(harness):
    """various/contracts/address_code.sol"""
    app = harness.compile_and_deploy("various/contracts/address_code.sol")
    # initCode() -> 0x20, 0
    r = harness.call(app, "initCode()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # h() -> 0
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 0

def test_address_code_complex(harness):
    """various/contracts/address_code_complex.sol"""
    app = harness.compile_and_deploy("various/contracts/address_code_complex.sol")
    # f() -> 0x20, 0x20, 0x48aa5566000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 20453482083385344)
    # g() -> 0x20
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 32

def test_assignment_to_const_var_involving_expression(harness):
    """various/contracts/assignment_to_const_var_involving_expression.sol"""
    app = harness.compile_and_deploy("various/contracts/assignment_to_const_var_involving_expression.sol")
    # f() -> 0x57a
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1402

def test_balance(harness):
    """various/contracts/balance.sol"""
    app = harness.compile_and_deploy("various/contracts/balance.sol", fund_wei=23)
    # getBalance() -> 23
    r = harness.call(app, "getBalance()")
    assert as_int(r.abi_return) == 23

def test_byte_optimization_bug(harness):
    """various/contracts/byte_optimization_bug.sol"""
    app = harness.compile_and_deploy("various/contracts/byte_optimization_bug.sol")
    # f(uint256): 2 -> 0
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 0
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 2

def test_code_access_content(harness):
    """various/contracts/code_access_content.sol"""
    app = harness.compile_and_deploy("various/contracts/code_access_content.sol")
    # testRuntime() -> true
    r = harness.call(app, "testRuntime()")
    assert bool(as_int(r.abi_return)) is True
    # testCreation() -> true
    r = harness.call(app, "testCreation()")
    assert bool(as_int(r.abi_return)) is True

def test_code_access_create(harness):
    """various/contracts/code_access_create.sol"""
    app = harness.compile_and_deploy("various/contracts/code_access_create.sol")
    # test() -> 7
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 7

def test_code_access_padding(harness):
    """various/contracts/code_access_padding.sol"""
    app = harness.compile_and_deploy("various/contracts/code_access_padding.sol")
    # diff() -> 0 # This checks that the allocation function pads to multiples of 32 bytes #
    r = harness.call(app, "diff()")
    # TODO: verify expected: 0 # This checks that the allocation function pads to multiples of 32 bytes #
    assert not r.reverted

def test_code_access_runtime(harness):
    """various/contracts/code_access_runtime.sol"""
    app = harness.compile_and_deploy("various/contracts/code_access_runtime.sol")
    # test() -> 42
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 42

def test_code_length(harness):
    """various/contracts/code_length.sol"""
    app = harness.compile_and_deploy("various/contracts/code_length.sol")
    # f(): true, true -> true, true
    r = harness.call(app, "f()", True, True)
    assert tuple(bool(b) for b in r.abi_return) == (True, True)

def test_code_length_contract_member(harness):
    """various/contracts/code_length_contract_member.sol"""
    app = harness.compile_and_deploy("various/contracts/code_length_contract_member.sol")
    # f() -> 0x20, 0x20, true
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x20 | 0x20 | true
    assert not r.reverted

def test_codebalance_assembly(harness):
    """various/contracts/codebalance_assembly.sol"""
    app = harness.compile_and_deploy("various/contracts/codebalance_assembly.sol", fund_wei=23)
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 1
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1
    # h() -> 23
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 23

def test_codehash(harness):
    """various/contracts/codehash.sol"""
    app = harness.compile_and_deploy("various/contracts/codehash.sol")
    # f() -> 0x0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 89477152217924674838424037953991966239322087453347756267410168184682657981552
    # h() -> true
    r = harness.call(app, "h()")
    assert bool(as_int(r.abi_return)) is True

def test_codehash_assembly(harness):
    """various/contracts/codehash_assembly.sol"""
    app = harness.compile_and_deploy("various/contracts/codehash_assembly.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 89477152217924674838424037953991966239322087453347756267410168184682657981552
    # h() -> true
    r = harness.call(app, "h()")
    assert bool(as_int(r.abi_return)) is True

def test_contract_binary_dependencies(harness):
    """various/contracts/contract_binary_dependencies.sol"""
    app = harness.compile_and_deploy("various/contracts/contract_binary_dependencies.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_crazy_elementary_typenames_on_stack(harness):
    """various/contracts/crazy_elementary_typenames_on_stack.sol"""
    app = harness.compile_and_deploy("various/contracts/crazy_elementary_typenames_on_stack.sol")
    # f() -> -7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) in (-7, 115792089237316195423570985008687907853269984665640564039457584007913129639929)

def test_create_calldata(harness):
    """various/contracts/create_calldata.sol"""
    app = harness.compile_and_deploy("various/contracts/create_calldata.sol", ctor_args=[42])
    # s() -> 0x20, 0
    r = harness.call(app, "s()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)

def test_create_random(harness):
    """various/contracts/create_random.sol"""
    app = harness.compile_and_deploy("various/contracts/create_random.sol")
    # addr() -> 0xc06afe3a8444fc0004668591e8306bfb9968e79e
    r = harness.call(app, "addr()")
    assert as_int(r.abi_return) == 1098512253422041666021416798982440481960491542430
    # testRunner() -> 0x137aa4dfc0911524504fcd4d98501f179bc13b4a, 0x2c1c30623ddd93e0b765a6caaca0c859eeb0644d
    r = harness.call(app, "testRunner()")
    assert tuple(as_int(x) for x in r.abi_return) == (111205878113699406076286690203704286544989862730, 251824229601437883924724639193039206405335180365)
    # testCalc() -> 0x137aa4dfc0911524504fcd4d98501f179bc13b4a, 0x2c1c30623ddd93e0b765a6caaca0c859eeb0644d
    r = harness.call(app, "testCalc()")
    assert tuple(as_int(x) for x in r.abi_return) == (111205878113699406076286690203704286544989862730, 251824229601437883924724639193039206405335180365)

def test_cross_contract_types(harness):
    """various/contracts/cross_contract_types.sol"""
    app = harness.compile_and_deploy("various/contracts/cross_contract_types.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3

def test_decayed_tuple(harness):
    """various/contracts/decayed_tuple.sol"""
    app = harness.compile_and_deploy("various/contracts/decayed_tuple.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_destructuring_assignment(harness):
    """various/contracts/destructuring_assignment.sol"""
    app = harness.compile_and_deploy("various/contracts/destructuring_assignment.sol")
    # f(bytes): 0x20, 0x5, "abcde" -> 0
    r = harness.call(app, "f(bytes)", 'abcde')
    assert as_int(r.abi_return) == 0

def test_different_call_type_transient(harness):
    """various/contracts/different_call_type_transient.sol"""
    app = harness.compile_and_deploy("various/contracts/different_call_type_transient.sol")
    # testDelegate() -> 7, 0
    r = harness.call(app, "testDelegate()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 0)
    # testCall() -> 0, 8
    r = harness.call(app, "testCall()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 8)
    # testStatic() -> false
    r = harness.call(app, "testStatic()")
    assert bool(as_int(r.abi_return)) is False

def test_empty_name_return_parameter(harness):
    """various/contracts/empty_name_return_parameter.sol"""
    app = harness.compile_and_deploy("various/contracts/empty_name_return_parameter.sol")
    # f(uint256): 9 -> 9
    r = harness.call(app, "f(uint256)", 9)
    assert as_int(r.abi_return) == 9

def test_erc20(harness):
    """various/contracts/erc20.sol"""
    app = harness.compile_and_deploy("various/contracts/erc20.sol")
    # totalSupply() -> 20
    r = harness.call(app, "totalSupply()")
    assert as_int(r.abi_return) == 20
    # transfer(address,uint256): 2, 5 -> true
    r = harness.call(app, "transfer(address,uint256)", 2, 5)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 0 -> true
    r = harness.call(app, "decreaseAllowance(address,uint256)", 2, 0)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "decreaseAllowance(address,uint256)", 2, 1, expect_revert=True)
    assert r.reverted
    # transfer(address,uint256): 2, 14 -> true
    r = harness.call(app, "transfer(address,uint256)", 2, 14)
    assert bool(as_int(r.abi_return)) is True
    # transfer(address,uint256): 2, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "transfer(address,uint256)", 2, 2, expect_revert=True)
    assert r.reverted

def test_external_types_in_calls(harness):
    """various/contracts/external_types_in_calls.sol"""
    app = harness.compile_and_deploy("various/contracts/external_types_in_calls.sol")
    # test() -> 9, 7
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (9, 7)
    # t2() -> 9
    r = harness.call(app, "t2()")
    assert as_int(r.abi_return) == 9

def test_flipping_sign_tests(harness):
    """various/contracts/flipping_sign_tests.sol"""
    app = harness.compile_and_deploy("various/contracts/flipping_sign_tests.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_gasleft_decrease(harness):
    """various/contracts/gasleft_decrease.sol"""
    app = harness.compile_and_deploy("various/contracts/gasleft_decrease.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # g() -> true
    r = harness.call(app, "g()")
    assert bool(as_int(r.abi_return)) is True

def test_gasleft_shadow_resolution(harness):
    """various/contracts/gasleft_shadow_resolution.sol"""
    app = harness.compile_and_deploy("various/contracts/gasleft_shadow_resolution.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_inline_member_init(harness):
    """various/contracts/inline_member_init.sol"""
    app = harness.compile_and_deploy("various/contracts/inline_member_init.sol")
    # get() -> 5, 6, 8
    r = harness.call(app, "get()")
    assert tuple(as_int(x) for x in r.abi_return) == (5, 6, 8)

def test_inline_member_init_inheritence(harness):
    """various/contracts/inline_member_init_inheritence.sol"""
    app = harness.compile_and_deploy("various/contracts/inline_member_init_inheritence.sol")
    # getBMember() -> 5
    r = harness.call(app, "getBMember()")
    assert as_int(r.abi_return) == 5
    # getDMember() -> 6
    r = harness.call(app, "getDMember()")
    assert as_int(r.abi_return) == 6

def test_inline_tuple_with_rational_numbers(harness):
    """various/contracts/inline_tuple_with_rational_numbers.sol"""
    app = harness.compile_and_deploy("various/contracts/inline_tuple_with_rational_numbers.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_iszero_bnot_correct(harness):
    """various/contracts/iszero_bnot_correct.sol"""
    app = harness.compile_and_deploy("various/contracts/iszero_bnot_correct.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_literal_empty_string(harness):
    """various/contracts/literal_empty_string.sol"""
    app = harness.compile_and_deploy("various/contracts/literal_empty_string.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # a() -> 0
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0
    # g() ->
    r = harness.call(app, "g()")
    # (void return — call succeeding is the assertion)
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # a() -> 2
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 2

def test_many_subassemblies(harness):
    """various/contracts/many_subassemblies.sol"""
    app = harness.compile_and_deploy("various/contracts/many_subassemblies.sol")
    # run() ->
    r = harness.call(app, "run()")
    # (void return — call succeeding is the assertion)

def test_memory_overwrite(harness):
    """various/contracts/memory_overwrite.sol"""
    app = harness.compile_and_deploy("various/contracts/memory_overwrite.sol")
    # f() -> 0x20, 5, "b23a5"
    r = harness.call(app, "f()")
    assert r.abi_return == 'b23a5'

def test_multi_modifiers(harness):
    """various/contracts/multi_modifiers.sol"""
    app = harness.compile_and_deploy("various/contracts/multi_modifiers.sol")
    # f1() ->
    r = harness.call(app, "f1()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x08
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 8
    # f2() ->
    r = harness.call(app, "f2()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x0c
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 12

def test_multi_variable_declaration(harness):
    """various/contracts/multi_variable_declaration.sol"""
    app = harness.compile_and_deploy("various/contracts/multi_variable_declaration.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_negative_stack_height(harness):
    """various/contracts/negative_stack_height.sol"""
    app = harness.compile_and_deploy("various/contracts/negative_stack_height.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_nested_calldata_struct(harness):
    """various/contracts/nested_calldata_struct.sol"""
    app = harness.compile_and_deploy("various/contracts/nested_calldata_struct.sol")
    # f((uint256,uint256,(uint256,uint256),uint256)): 1, 2, 3, 4, 5 -> 1, 2, 3, 4, 5
    r = harness.call(app, "f((uint256,uint256,(uint256,uint256),uint256))", 1, 2, 3, 4, 5)
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5
    assert not r.reverted

def test_nested_calldata_struct_to_memory(harness):
    """various/contracts/nested_calldata_struct_to_memory.sol"""
    app = harness.compile_and_deploy("various/contracts/nested_calldata_struct_to_memory.sol")
    # f((uint256,uint256,(uint256,uint256),uint256)): 1, 2, 3, 4, 5 -> 1, 2, 3, 4, 5
    r = harness.call(app, "f((uint256,uint256,(uint256,uint256),uint256))", 1, 2, 3, 4, 5)
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5
    assert not r.reverted

def test_positive_integers_to_signed(harness):
    """various/contracts/positive_integers_to_signed.sol"""
    app = harness.compile_and_deploy("various/contracts/positive_integers_to_signed.sol")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # y() -> 127
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 127
    # q() -> 250
    r = harness.call(app, "q()")
    assert as_int(r.abi_return) == 250

def test_selfdestruct_post_cancun(harness):
    """various/contracts/selfdestruct_post_cancun.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun.sol", fund_wei=1000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_create_and_terminate() ->
    r = harness.call(app, "test_create_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # terminate() -> FAILURE
    r = harness.call(app, "terminate()", expect_revert=True)
    assert r.reverted
    # deploy_create() ->
    r = harness.call(app, "deploy_create()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # test_create2_and_terminate() ->
    r = harness.call(app, "test_create2_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy_create2() ->
    r = harness.call(app, "deploy_create2()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)

def test_selfdestruct_post_cancun_multiple_beneficiaries(harness):
    """various/contracts/selfdestruct_post_cancun_multiple_beneficiaries.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun_multiple_beneficiaries.sol", fund_wei=2000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_deploy_and_terminate_twice() ->
    r = harness.call(app, "test_deploy_and_terminate_twice()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy() ->
    r = harness.call(app, "deploy()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate(address): 0x1111111111111111111111111111111111111111 ->
    r = harness.call(app, "terminate(address)", 0x1111111111111111111111111111111111111111)
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True

def test_selfdestruct_post_cancun_redeploy(harness):
    """various/contracts/selfdestruct_post_cancun_redeploy.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun_redeploy.sol", fund_wei=1000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_deploy_and_terminate() ->
    r = harness.call(app, "test_deploy_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy_create2() ->
    r = harness.call(app, "deploy_create2()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # deploy_create2() -> FAILURE
    r = harness.call(app, "deploy_create2()", expect_revert=True)
    assert r.reverted

def test_selfdestruct_pre_cancun(harness):
    """various/contracts/selfdestruct_pre_cancun.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun.sol", evm_version='shanghai', fund_wei=1000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_create_and_terminate() ->
    r = harness.call(app, "test_create_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # terminate() -> FAILURE
    r = harness.call(app, "terminate()", expect_revert=True)
    assert r.reverted
    # deploy_create() ->
    r = harness.call(app, "deploy_create()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_create2_and_terminate() ->
    r = harness.call(app, "test_create2_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy_create2() ->
    r = harness.call(app, "deploy_create2()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # terminate() -> FAILURE
    r = harness.call(app, "terminate()", expect_revert=True)
    assert r.reverted

def test_selfdestruct_pre_cancun_multiple_beneficiaries(harness):
    """various/contracts/selfdestruct_pre_cancun_multiple_beneficiaries.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun_multiple_beneficiaries.sol", evm_version='shanghai', fund_wei=2000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_deploy_and_terminate_twice() ->
    r = harness.call(app, "test_deploy_and_terminate_twice()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy() ->
    r = harness.call(app, "deploy()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate(address): 0x1111111111111111111111111111111111111111 ->
    r = harness.call(app, "terminate(address)", 0x1111111111111111111111111111111111111111)
    # (void return — call succeeding is the assertion)
    # terminate(address): 0x2222222222222222222222222222222222222222 -> FAILURE
    r = harness.call(app, "terminate(address)", 0x2222222222222222222222222222222222222222, expect_revert=True)
    assert r.reverted
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False

def test_selfdestruct_pre_cancun_redeploy(harness):
    """various/contracts/selfdestruct_pre_cancun_redeploy.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun_redeploy.sol", evm_version='shanghai', fund_wei=1000000000000000000)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # test_deploy_and_terminate() ->
    r = harness.call(app, "test_deploy_and_terminate()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy_create2() ->
    r = harness.call(app, "deploy_create2()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_create() ->
    r = harness.call(app, "test_balance_after_create()")
    # (void return — call succeeding is the assertion)
    # exists() -> true
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is True
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)
    # test_balance_after_selfdestruct() ->
    r = harness.call(app, "test_balance_after_selfdestruct()")
    # (void return — call succeeding is the assertion)
    # exists() -> false
    r = harness.call(app, "exists()")
    assert bool(as_int(r.abi_return)) is False
    # deploy_create2() ->
    r = harness.call(app, "deploy_create2()")
    # (void return — call succeeding is the assertion)

def test_senders_balance(harness):
    """various/contracts/senders_balance.sol"""
    app = harness.compile_and_deploy("various/contracts/senders_balance.sol", fund_wei=27)
    # f() -> 27
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 27

def test_single_copy_with_multiple_inheritance(harness):
    """various/contracts/single_copy_with_multiple_inheritance.sol"""
    app = harness.compile_and_deploy("various/contracts/single_copy_with_multiple_inheritance.sol")
    # getViaB() -> 0
    r = harness.call(app, "getViaB()")
    assert as_int(r.abi_return) == 0
    # setViaA(uint256): 23 ->
    r = harness.call(app, "setViaA(uint256)", 23)
    # (void return — call succeeding is the assertion)
    # getViaB() -> 23
    r = harness.call(app, "getViaB()")
    assert as_int(r.abi_return) == 23

def test_skip_dynamic_types(harness):
    """various/contracts/skip_dynamic_types.sol"""
    app = harness.compile_and_deploy("various/contracts/skip_dynamic_types.sol")
    # g() -> 7, 8
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)

def test_skip_dynamic_types_for_static_arrays_with_dynamic_elements(harness):
    """various/contracts/skip_dynamic_types_for_static_arrays_with_dynamic_elements.sol"""
    app = harness.compile_and_deploy("various/contracts/skip_dynamic_types_for_static_arrays_with_dynamic_elements.sol")
    # g() -> 5, 6
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (5, 6)

def test_skip_dynamic_types_for_structs(harness):
    """various/contracts/skip_dynamic_types_for_structs.sol"""
    app = harness.compile_and_deploy("various/contracts/skip_dynamic_types_for_structs.sol")
    # g() -> 2, 6
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 6)

def test_state_variable_local_variable_mixture(harness):
    """various/contracts/state_variable_local_variable_mixture.sol"""
    app = harness.compile_and_deploy("various/contracts/state_variable_local_variable_mixture.sol")
    # a() -> 2
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 2

def test_state_variable_under_contract_name(harness):
    """various/contracts/state_variable_under_contract_name.sol"""
    app = harness.compile_and_deploy("various/contracts/state_variable_under_contract_name.sol")
    # getStateVar() -> 42
    r = harness.call(app, "getStateVar()")
    assert as_int(r.abi_return) == 42

def test_staticcall_for_view_and_pure(harness):
    """various/contracts/staticcall_for_view_and_pure.sol"""
    app = harness.compile_and_deploy("various/contracts/staticcall_for_view_and_pure.sol")
    # f() -> 0x1 # This should work, next should throw #
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x1 # This should work | next should throw #
    assert not r.reverted
    # fview() -> FAILURE
    r = harness.call(app, "fview()", expect_revert=True)
    assert r.reverted
    # fpure() -> FAILURE
    r = harness.call(app, "fpure()", expect_revert=True)
    assert r.reverted

def test_staticcall_for_view_and_pure_pre_byzantium(harness):
    """various/contracts/staticcall_for_view_and_pure_pre_byzantium.sol"""
    app = harness.compile_and_deploy("various/contracts/staticcall_for_view_and_pure_pre_byzantium.sol", evm_version='spuriousDragon')
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # fview() -> 1
    r = harness.call(app, "fview()")
    assert as_int(r.abi_return) == 1
    # fpure() -> 1
    r = harness.call(app, "fpure()")
    assert as_int(r.abi_return) == 1

def test_storage_string_as_mapping_key_without_variable(harness):
    """various/contracts/storage_string_as_mapping_key_without_variable.sol"""
    app = harness.compile_and_deploy("various/contracts/storage_string_as_mapping_key_without_variable.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_store_bytes(harness):
    """various/contracts/store_bytes.sol"""
    app = harness.compile_and_deploy("various/contracts/store_bytes.sol")
    # save() -> 24 # empty copy loop #
    r = harness.call(app, "save()")
    # TODO: verify expected: 24 # empty copy loop #
    assert not r.reverted
    # save(): "abcdefg" -> 24
    r = harness.call(app, "save()", bytes.fromhex('61626364656667'))
    assert as_int(r.abi_return) == 24

def test_string_tuples(harness):
    """various/contracts/string_tuples.sol"""
    app = harness.compile_and_deploy("various/contracts/string_tuples.sol")
    # f() -> 0x40, 0x8, 0x3, "abc"
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x40 | 0x8 | 0x3 | "abc"
    assert not r.reverted
    # g() -> 0x40, 0x80, 0x3, "abc", 0x3, "def"
    r = harness.call(app, "g()")
    # TODO: verify expected: 0x40 | 0x80 | 0x3 | "abc" | 0x3 | "def"
    assert not r.reverted

def test_super(harness):
    """various/contracts/super.sol"""
    app = harness.compile_and_deploy("various/contracts/super.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 15

def test_super_alone(harness):
    """various/contracts/super_alone.sol"""
    app = harness.compile_and_deploy("various/contracts/super_alone.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_super_parentheses(harness):
    """various/contracts/super_parentheses.sol"""
    app = harness.compile_and_deploy("various/contracts/super_parentheses.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 15

def test_swap_in_storage_overwrite(harness):
    """various/contracts/swap_in_storage_overwrite.sol"""
    app = harness.compile_and_deploy("various/contracts/swap_in_storage_overwrite.sol")
    # x() -> 0, 0
    r = harness.call(app, "x()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # y() -> 0, 0
    r = harness.call(app, "y()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # x() -> 1, 2
    r = harness.call(app, "x()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # y() -> 3, 4
    r = harness.call(app, "y()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 4)
    # swap() ->
    r = harness.call(app, "swap()")
    # (void return — call succeeding is the assertion)
    # x() -> 1, 2
    r = harness.call(app, "x()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # y() -> 1, 2
    r = harness.call(app, "y()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_test_underscore_in_hex(harness):
    """various/contracts/test_underscore_in_hex.sol"""
    app = harness.compile_and_deploy("various/contracts/test_underscore_in_hex.sol")
    # f(bool): true -> 0x1234ab
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1193131
    # f(bool): false -> 0x1234abcd1234
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 20017429942836

def test_transient_storage_reentrancy_lock(harness):
    """various/contracts/transient_storage_reentrancy_lock.sol"""
    app = harness.compile_and_deploy("various/contracts/transient_storage_reentrancy_lock.sol")
    # test(address,bool): 0x1234abcd, true -> FAILURE, hex"08c379a0", 0x20, 0x12, "Reentrancy attempt"
    r = harness.call(app, "test(address,bool)", 305441741, True, expect_revert=True)
    assert r.reverted
    # test(address,bool): 0x1234abcd, false ->
    r = harness.call(app, "test(address,bool)", 305441741, False)
    # (void return — call succeeding is the assertion)

def test_tuples(harness):
    """various/contracts/tuples.sol"""
    app = harness.compile_and_deploy("various/contracts/tuples.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_typed_multi_variable_declaration(harness):
    """various/contracts/typed_multi_variable_declaration.sol"""
    app = harness.compile_and_deploy("various/contracts/typed_multi_variable_declaration.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_write_storage_external(harness):
    """various/contracts/write_storage_external.sol"""
    app = harness.compile_and_deploy("various/contracts/write_storage_external.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # g() -> 8
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 8
    # h() -> 12
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 12
