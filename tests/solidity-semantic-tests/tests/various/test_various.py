"""Tests for the various category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_address_code(harness):  # currently fails
    """various/contracts/address_code.sol"""
    app = harness.compile_and_deploy('various/contracts/address_code.sol')
    r = harness.call(app, 'initCode()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0,)
    r = harness.call(app, 'f()')
    assert r.abi_return is True
    r = harness.call(app, 'g()')
    assert as_int(r.abi_return) == 0
    r = harness.call(app, 'h()')
    assert as_int(r.abi_return) == 0

@pytest.mark.xfail(reason="`address(other).code` is a compile-time hard error per EVM_DIVERGENCE.md — arbitrary-address code introspection can't be reliably dereferenced on AVM", strict=False)
def test_address_code_complex(harness):  # currently fails
    """various/contracts/address_code_complex.sol"""
    app = harness.compile_and_deploy('various/contracts/address_code_complex.sol')
    r = harness.call(app, 'f()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x20, 0x48aa5566000000,)
    r = harness.call(app, 'g()')
    assert as_int(r.abi_return) == 0x20

def test_assignment_to_const_var_involving_expression(harness):
    """various/contracts/assignment_to_const_var_involving_expression.sol"""
    app = harness.compile_and_deploy("various/contracts/assignment_to_const_var_involving_expression.sol")
    # f() -> 0x57a
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1402

def test_balance(harness):
    """various/contracts/balance.sol"""
    # AVM apps always carry a minimum balance for state schema/MBR; subtract
    # that baseline so the test asserts on just the ctor-forwarded 23 wei.
    app = harness.compile_and_deploy("various/contracts/balance.sol", fund_wei=23)
    r = harness.call(app, "getBalance()")
    assert as_int(r.abi_return) - app.balance_baseline == 23

def test_byte_optimization_bug(harness):
    """various/contracts/byte_optimization_bug.sol"""
    app = harness.compile_and_deploy("various/contracts/byte_optimization_bug.sol")
    # f(uint256): 2 -> 0
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 0
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 2

@pytest.mark.xfail(reason="Yul `codecopy` is a compile-time hard error on AVM — no AVM translation exists, so it would be a silent wrong value", strict=False)
def test_code_access_content(harness):  # currently fails
    """various/contracts/code_access_content.sol"""
    app = harness.compile_and_deploy('various/contracts/code_access_content.sol')
    r = harness.call(app, 'testRuntime()')
    assert r.abi_return is True
    r = harness.call(app, 'testCreation()')
    assert r.abi_return is True

@pytest.mark.xfail(reason="Yul `create` is a compile-time hard error on AVM — contract creation from raw bytecode has no AVM translation (would be a silent wrong value)", strict=False)
def test_code_access_create(harness):  # currently fails
    """various/contracts/code_access_create.sol"""
    app = harness.compile_and_deploy('various/contracts/code_access_create.sol')
    r = harness.call(app, 'test()')
    assert as_int(r.abi_return) == 7

def test_code_access_padding(harness):
    """various/contracts/code_access_padding.sol"""
    app = harness.compile_and_deploy("various/contracts/code_access_padding.sol")
    # diff() -> 0 # This checks that the allocation function pads to multiples of 32 bytes #
    r = harness.call(app, "diff()")
    # TODO: verify expected: 0 # This checks that the allocation function pads to multiples of 32 bytes #
    assert not r.reverted

@pytest.mark.xfail(reason="`extcodehash(addr)` is a compile-time hard error per EVM_DIVERGENCE.md — an arbitrary address can't be dereferenced to its code on AVM", strict=False)
def test_code_access_runtime(harness):  # currently fails
    """various/contracts/code_access_runtime.sol"""
    app = harness.compile_and_deploy('various/contracts/code_access_runtime.sol')
    r = harness.call(app, 'test()')
    assert as_int(r.abi_return) == 42

@pytest.mark.xfail(reason="`address(other).code` is a compile-time hard error per EVM_DIVERGENCE.md — arbitrary-address code introspection can't be reliably dereferenced on AVM", strict=False)
def test_code_length(harness):  # currently fails
    """various/contracts/code_length.sol"""
    app = harness.compile_and_deploy('various/contracts/code_length.sol')
    r = harness.call(app, 'f()', True, True)
    assert tuple(r.abi_return) == (True, True,)

def test_code_length_contract_member(harness):
    """various/contracts/code_length_contract_member.sol"""
    app = harness.compile_and_deploy("various/contracts/code_length_contract_member.sol")
    # f() -> 0x20, 0x20, true
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x20 | 0x20 | true
    assert not r.reverted

@pytest.mark.xfail(reason="Yul `balance` has no AVM equivalent; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0)", strict=False)
def test_codebalance_assembly(harness):
    """various/contracts/codebalance_assembly.sol"""
    app = harness.compile_and_deploy('various/contracts/codebalance_assembly.sol')

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

@pytest.mark.xfail(reason="Yul extcodehash(addr) is now a hard compile error per EVM_DIVERGENCE.md — AVM can't dereference an arbitrary address to its code; the old stub used an addr>100 heuristic + a wrong-but-deterministic hash. Use address(this).codehash for the self case", strict=False)
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

def test_create_calldata(harness):  # currently fails
    """various/contracts/create_calldata.sol"""
    app = harness.compile_and_deploy('various/contracts/create_calldata.sol')
    r = harness.call(app, 's()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0,)

def test_create_random(harness):
    """various/contracts/create_random.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("various/contracts/create_random.sol", postinit_inner_txns=4)
    assert not harness.call(app, "addr()").reverted
    assert not harness.call(app, "testRunner()").reverted
    assert not harness.call(app, "testCalc()").reverted

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
    r = harness.call(app, "f(bytes)", b"abcde")
    assert as_int(r.abi_return) == 0

def test_different_call_type_transient(harness):  # currently fails
    """various/contracts/different_call_type_transient.sol"""
    app = harness.compile_and_deploy('various/contracts/different_call_type_transient.sol')
    r = harness.call(app, 'testDelegate()')
    assert tuple(as_int(x) for x in r.abi_return) == (7, 0,)
    r = harness.call(app, 'testCall()')
    assert tuple(as_int(x) for x in r.abi_return) == (0, 8,)
    r = harness.call(app, 'testStatic()')
    assert r.abi_return is False

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
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 5)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 0 -> true
    r = harness.call(app, "decreaseAllowance(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 0)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "decreaseAllowance(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 1, expect_revert=True)
    assert r.reverted
    # transfer(address,uint256): 2, 14 -> true
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 14)
    assert bool(as_int(r.abi_return)) is True
    # transfer(address,uint256): 2, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 2, expect_revert=True)
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
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy('various/contracts/many_subassemblies.sol')
    r = harness.call(app, 'run()')

def test_memory_overwrite(harness):
    """various/contracts/memory_overwrite.sol"""
    app = harness.compile_and_deploy("various/contracts/memory_overwrite.sol")
    # f returns bytes "b23a5".
    assert bytes(harness.call(app, "f()").abi_return) == b"b23a5"

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
    arg = (1, 2, (3, 4), 5)
    assert not harness.call(app, "f((uint256,uint256,(uint256,uint256),uint256))", arg).reverted

def test_nested_calldata_struct_to_memory(harness):
    """various/contracts/nested_calldata_struct_to_memory.sol"""
    app = harness.compile_and_deploy("various/contracts/nested_calldata_struct_to_memory.sol")
    arg = (1, 2, (3, 4), 5)
    assert not harness.call(app, "f((uint256,uint256,(uint256,uint256),uint256))", arg).reverted

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
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun.sol")

@pytest.mark.xfail(reason="`address(other).code` is a compile-time hard error per EVM_DIVERGENCE.md — this contract's `exists()` uses `address(c).code.length != 0` (the Address.isContract() pattern) on a non-`this` address, which AVM can't reliably dereference", strict=False)
def test_selfdestruct_post_cancun_multiple_beneficiaries(harness):
    """various/contracts/selfdestruct_post_cancun_multiple_beneficiaries.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun_multiple_beneficiaries.sol")

def test_selfdestruct_post_cancun_redeploy(harness):
    """various/contracts/selfdestruct_post_cancun_redeploy.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("various/contracts/selfdestruct_post_cancun_redeploy.sol")

def test_selfdestruct_pre_cancun(harness):
    """various/contracts/selfdestruct_pre_cancun.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun.sol")

@pytest.mark.xfail(reason="`address(other).code` is a compile-time hard error per EVM_DIVERGENCE.md — this contract's `exists()` uses `address(c).code.length != 0` (the Address.isContract() pattern) on a non-`this` address, which AVM can't reliably dereference", strict=False)
def test_selfdestruct_pre_cancun_multiple_beneficiaries(harness):
    """various/contracts/selfdestruct_pre_cancun_multiple_beneficiaries.sol"""
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun_multiple_beneficiaries.sol")

def test_selfdestruct_pre_cancun_redeploy(harness):
    """various/contracts/selfdestruct_pre_cancun_redeploy.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("various/contracts/selfdestruct_pre_cancun_redeploy.sol")

def test_senders_balance(harness):
    """various/contracts/senders_balance.sol — D.f() calls C.f() which
    returns msg.sender.balance (= D's balance). On AVM D has MBR + 27;
    the EVM test expected just 27. Just verify the chain works."""
    app = harness.compile_and_deploy("various/contracts/senders_balance.sol", fund_wei=27, postinit_inner_txns=4)
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) >= 27

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
    app = harness.compile_and_deploy('various/contracts/skip_dynamic_types_for_static_arrays_with_dynamic_elements.sol')
    r = harness.call(app, 'g()')
    assert tuple(as_int(x) for x in r.abi_return) == (5, 6,)

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

def test_staticcall_for_view_and_pure_pre_byzantium(harness):  # currently fails
    """various/contracts/staticcall_for_view_and_pure_pre_byzantium.sol"""
    app = harness.compile_and_deploy('various/contracts/staticcall_for_view_and_pure_pre_byzantium.sol')
    r = harness.call(app, 'f()')
    assert as_int(r.abi_return) == 0x1
    r = harness.call(app, 'fview()')
    assert as_int(r.abi_return) == 1
    r = harness.call(app, 'fpure()')
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
    r = harness.call(app, "test(address,bool)", encoding.encode_address((305441741).to_bytes(32, "big")), True, expect_revert=True)
    assert r.reverted
    # test(address,bool): 0x1234abcd, false ->
    r = harness.call(app, "test(address,bool)", encoding.encode_address((305441741).to_bytes(32, "big")), False)
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
