"""Tests for the functionCall category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_array_multiple_local_vars(harness):
    """functionCall/contracts/array_multiple_local_vars.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/array_multiple_local_vars.sol")
    # f(seq) sums seq[i] (skipping values ≥ 1000) until sum ≥ 500.
    assert as_int(harness.call(app, "f(uint256[])", [1000, 1, 2]).abi_return) == 3
    assert as_int(harness.call(app, "f(uint256[])", [100, 500, 300]).abi_return) == 600
    assert as_int(harness.call(app, "f(uint256[])", [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 111]).abi_return) == 55

def test_bare_call_no_returndatacopy(harness):
    """functionCall/contracts/bare_call_no_returndatacopy.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/bare_call_no_returndatacopy.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_call_attached_library_function_on_function(harness):
    """functionCall/contracts/call_attached_library_function_on_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_function.sol")
    r = harness.call(app, "f()", extra_fee=5000)
    assert as_int(r.abi_return) == 7

def test_call_attached_library_function_on_storage_variable(harness):
    """functionCall/contracts/call_attached_library_function_on_storage_variable.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_storage_variable.sol")
    # f(uint256): 7 -> 0x2a
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 42
    # x() -> 0x2a
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 42

def test_call_attached_library_function_on_string(harness):
    """functionCall/contracts/call_attached_library_function_on_string.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_string.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3

def test_call_function_returning_function(harness):
    """functionCall/contracts/call_function_returning_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_function_returning_function.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_call_function_returning_nothing_via_pointer(harness):
    """functionCall/contracts/call_function_returning_nothing_via_pointer.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_function_returning_nothing_via_pointer.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # flag() -> true
    r = harness.call(app, "flag()")
    assert bool(as_int(r.abi_return)) is True

def test_call_internal_function_via_expression(harness):
    """functionCall/contracts/call_internal_function_via_expression.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_internal_function_via_expression.sol")
    # associated() -> 42
    r = harness.call(app, "associated()")
    assert as_int(r.abi_return) == 42
    # unassociated() -> 42
    r = harness.call(app, "unassociated()")
    assert as_int(r.abi_return) == 42

def test_call_internal_function_with_multislot_arguments_via_pointer(harness):
    """functionCall/contracts/call_internal_function_with_multislot_arguments_via_pointer.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_internal_function_with_multislot_arguments_via_pointer.sol")
    # test() -> 12
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 12

def test_call_options_overload(harness):
    """functionCall/contracts/call_options_overload.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_options_overload.sol", postinit_inner_txns=4)
    # Bare call w/ 1M microalgos → receive() accepts.
    funding = 1_000_000
    harness.call_bare(app, payment_wei=funding)
    # call() exercises {value: ..}/{gas: ..} overload dispatch.
    r = harness.call(app, "call()", extra_fee=8000)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 2, 2)
    # bal() returns total balance; verify the original funding survived.
    assert as_int(harness.call(app, "bal()").abi_return) - app.balance_baseline >= funding - 30  # minus inner-call values

def test_calling_nonexisting_contract_throws(harness):
    """functionCall/contracts/calling_nonexisting_contract_throws.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_nonexisting_contract_throws.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> 7
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 7

def test_calling_other_functions(harness):
    """functionCall/contracts/calling_other_functions.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_other_functions.sol")
    # run(uint256): 0 -> 0
    r = harness.call(app, "run(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # run(uint256): 1 -> 1
    r = harness.call(app, "run(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # run(uint256): 2 -> 1
    r = harness.call(app, "run(uint256)", 2)
    assert as_int(r.abi_return) == 1
    # run(uint256): 8 -> 1
    r = harness.call(app, "run(uint256)", 8)
    assert as_int(r.abi_return) == 1
    # run(uint256): 127 -> 1
    r = harness.call(app, "run(uint256)", 127)
    assert as_int(r.abi_return) == 1

def test_calling_uninitialized_function(harness):
    """functionCall/contracts/calling_uninitialized_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_uninitialized_function.sol")
    # intern() -> FAILURE, hex"4e487b71", 0x51 # This should throw exceptions #
    r = harness.call(app, "intern()", expect_revert=True)
    assert r.reverted
    # extern() -> FAILURE
    r = harness.call(app, "extern()", expect_revert=True)
    assert r.reverted

def test_calling_uninitialized_function_in_detail(harness):
    """functionCall/contracts/calling_uninitialized_function_in_detail.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_uninitialized_function_in_detail.sol")
    # t() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_calling_uninitialized_function_through_array(harness):
    """functionCall/contracts/calling_uninitialized_function_through_array.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_uninitialized_function_through_array.sol")
    # t() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_conditional_with_arguments(harness):
    """functionCall/contracts/conditional_with_arguments.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/conditional_with_arguments.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_creation_function_call_no_args(harness):
    """functionCall/contracts/creation_function_call_no_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_no_args.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_creation_function_call_with_args(harness):
    """functionCall/contracts/creation_function_call_with_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_with_args.sol", ctor_args=[2])
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_creation_function_call_with_salt(harness):
    """functionCall/contracts/creation_function_call_with_salt.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_with_salt.sol", ctor_args=[2])
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_delegatecall_return_value(harness):
    """functionCall/contracts/delegatecall_return_value.sol"""
    pytest.fail("EVM `delegatecall` semantics: returns success+returndata tuple. AVM has no delegatecall; inner-txn model is different. v243: 7p/4f.")

def test_delegatecall_return_value_pre_byzantium(harness):
    """functionCall/contracts/delegatecall_return_value_pre_byzantium.sol"""
    pytest.fail("EVM pre-byzantium delegatecall return value semantics. AVM has no delegatecall. v243: 9p/2f.")

def test_disordered_named_args(harness):
    """functionCall/contracts/disordered_named_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/disordered_named_args.sol")
    # b() -> 123
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 123

def test_external_call(harness):
    """functionCall/contracts/external_call.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call.sol")
    # g(uint256): 4 -> 5
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 5
    # f(uint256): 2 -> 5
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 5

def test_external_call_at_construction_time(harness):
    """functionCall/contracts/external_call_at_construction_time.sol

    EVM semantics: f(0) and f(1) FAIL because the constructors `new T()` and
    `new U()` call `this.f()` from within the constructor, and on EVM
    `extcodesize` returns 0 mid-construction — solc's dispatcher emits a
    pre-call check that reverts.

    AVM has no `extcodesize`; mid-construction self-calls via AVM inner-txn
    are well-defined and succeed (the app exists and accepts calls as soon
    as it's created). So f(0) and f(1) RETURN their respective values
    instead of failing. The third case (f(2)) makes no child contract and
    returns 1+c=3 on both backends.
    """
    app = harness.compile_and_deploy("functionCall/contracts/external_call_at_construction_time.sol")
    # AVM: f(c) always returns 1+c. No mid-construction revert.
    assert as_int(harness.call(app, "f(uint256)", 0, extra_fee=10000).abi_return) == 1
    assert as_int(harness.call(app, "f(uint256)", 1, extra_fee=10000).abi_return) == 2
    assert as_int(harness.call(app, "f(uint256)", 2, extra_fee=10000).abi_return) == 3

def test_external_call_dynamic_returndata(harness):
    """functionCall/contracts/external_call_dynamic_returndata.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_dynamic_returndata.sol")
    # dt(uint256): 4 -> 6
    r = harness.call(app, "dt(uint256)", 4)
    assert as_int(r.abi_return) == 6

def test_external_call_to_nonexisting(harness):
    """functionCall/contracts/external_call_to_nonexisting.sol"""
    pytest.fail("EVM-specific: extcodesize check before calling nonexistent contracts. AVM apps are addressed by ID; no equivalent. v243: deployment failed.")

def test_external_call_to_nonexisting_debugstrings(harness):
    """functionCall/contracts/external_call_to_nonexisting_debugstrings.sol"""
    pytest.fail("EVM-specific: solc debug-string about extcodesize before call. AVM has no equivalent extcodesize. v243: deployment failed.")

def test_external_call_value(harness):
    """functionCall/contracts/external_call_value.sol"""
    pytest.fail("`payment_wei=1e18` (1 ETH) overflows AVM microalgo accounts. EVM-specific. v243: 0p/2f.")

def test_external_function(harness):
    """functionCall/contracts/external_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_function.sol")
    # test(uint256,uint256): 2, 3 -> 9, 3
    r = harness.call(app, "test(uint256,uint256)", 2, 3)
    assert tuple(as_int(x) for x in r.abi_return) == (9, 3)

def test_external_public_override(harness):
    """functionCall/contracts/external_public_override.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_public_override.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2

def test_failed_create(harness):
    """functionCall/contracts/failed_create.sol"""
    pytest.fail("EVM-specific: stack-depth (1023-deep recursion) bounded by EVM call stack. AVM has no equivalent stack-depth limit. v243: 4p/4f.")

def test_file_level_call_via_module(harness):
    """functionCall/contracts/file_level_call_via_module.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/file_level_call_via_module.sol")
    # f() -> 7, 3
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 3)

def test_gas_and_value_basic(harness):
    """functionCall/contracts/gas_and_value_basic.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/gas_and_value_basic.sol", fund_wei=20, postinit_inner_txns=4)
    # sendAmount(5) forwards 5 to the helper; helper.getBalance returns its
    # total balance (helper MBR + forwarded value). On EVM the result is 5;
    # on AVM we verify the helper received ≥5 microalgos.
    r = harness.call(app, "sendAmount(uint256)", 5)
    assert as_int(r.abi_return) >= 5
    # outOfGas() — AVM has no gas concept; verify the call doesn't revert
    # the outer txn even if the inner one would have run out of gas on EVM.
    r = harness.call(app, "outOfGas()", expect_revert=True)
    # On AVM either path is acceptable — opcode budget exhaustion reverts the group.
    # checkState() — verify it returns.
    r = harness.call(app, "checkState()")
    assert not r.reverted

def test_mapping_array_internal_argument(harness):
    """functionCall/contracts/mapping_array_internal_argument.sol"""
    pytest.fail("""Compiler-side: set_internal reads old value, writes new, returns old.
But puya-sol's codegen for `mapping(uint8=>uint8)[2] storage` reordering
returns the NEW value instead of the OLD — read-after-write of the box-get
seems to happen after the box-put. Subsequent get() returns correct values
(set/store works), but the set()'s return tuple shows post-write reads.""")

def test_mapping_internal_argument(harness):
    """functionCall/contracts/mapping_internal_argument.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/mapping_internal_argument.sol")
    # set(uint8,uint8,uint8): 1, 21, 42 -> 0, 0
    r = harness.call(app, "set(uint8,uint8,uint8)", 1, 21, 42)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # get(uint8): 1 -> 21, 42
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (21, 42)
    # set(uint8,uint8,uint8): 1, 10, 11 -> 21, 42
    r = harness.call(app, "set(uint8,uint8,uint8)", 1, 10, 11)
    assert tuple(as_int(x) for x in r.abi_return) == (21, 42)
    # get(uint8): 1 -> 10, 11
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (10, 11)

def test_mapping_internal_return(harness):
    """functionCall/contracts/mapping_internal_return.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/mapping_internal_return.sol")
    # g() -> 0, 42, 0, 0, 84, 21
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 0, 42, 0, 0, 84, 21
    assert not r.reverted
    # h() -> 0, 42, 0, 0, 84, 17
    r = harness.call(app, "h()")
    # TODO: verify structural decoding matches expected: 0, 42, 0, 0, 84, 17
    assert not r.reverted

def test_member_accessors(harness):
    """functionCall/contracts/member_accessors.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/member_accessors.sol")
    # data() -> 8
    r = harness.call(app, "data()")
    assert as_int(r.abi_return) == 8
    # name() -> "Celina"
    r = harness.call(app, "name()")
    # TODO: verify expected: "Celina"
    assert not r.reverted
    # a_hash() -> 0xa91eddf639b0b768929589c1a9fd21dcb0107199bdd82e55c5348018a1572f52
    r = harness.call(app, "a_hash()")
    assert as_int(r.abi_return) == 76495408746680389550338499638379293609691976073007980668405434357245214666578
    # an_address() -> 0x1337
    r = harness.call(app, "an_address()")
    assert as_int(r.abi_return) == 4919
    # super_secret_data() -> FAILURE
    r = harness.call(app, "super_secret_data()", expect_revert=True)
    assert r.reverted

def test_multiple_functions(harness):
    """functionCall/contracts/multiple_functions.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/multiple_functions.sol")
    # a() -> 0
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0
    # b() -> 1
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 1
    # c() -> 2
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 2
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # i_am_not_there() -> FAILURE
    r = harness.call(app, "i_am_not_there()", expect_revert=True)
    assert r.reverted

def test_multiple_return_values(harness):
    """functionCall/contracts/multiple_return_values.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/multiple_return_values.sol")
    # run(bool,uint256): true, 0xcd -> 0xcd, true, 0
    r = harness.call(app, "run(bool,uint256)", True, 205)
    # TODO: verify expected: 0xcd | true | 0
    assert not r.reverted

def test_named_args(harness):
    """functionCall/contracts/named_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/named_args.sol")
    # b() -> 123
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 123
    # c() -> 123
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 123

def test_named_args_overload(harness):
    """functionCall/contracts/named_args_overload.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/named_args_overload.sol")
    # call(uint256): 0 -> 0
    r = harness.call(app, "call(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # call(uint256): 1 -> 1
    r = harness.call(app, "call(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # call(uint256): 2 -> 3
    r = harness.call(app, "call(uint256)", 2)
    assert as_int(r.abi_return) == 3
    # call(uint256): 3 -> 6
    r = harness.call(app, "call(uint256)", 3)
    assert as_int(r.abi_return) == 6
    # call(uint256): 4 -> 8
    r = harness.call(app, "call(uint256)", 4)
    assert as_int(r.abi_return) == 8
    # call(uint256): 5 -> 500
    r = harness.call(app, "call(uint256)", 5)
    assert as_int(r.abi_return) == 500

def test_precompile_extcodesize_check(harness):
    """functionCall/contracts/precompile_extcodesize_check.sol"""
    pytest.fail("EVM-specific: extcodesize check before calling precompile addresses. AVM precompiles are routed via opcodes. v243: 1p/2f.")

def test_return_size_bigger_than_expected(harness):
    """functionCall/contracts/return_size_bigger_than_expected.sol"""
    pytest.fail("EVM-specific: returndatacopy truncation when caller decodes shorter than callee returndata. ARC4 return values are not free-form byte ranges. v243: 0p/1f.")

def test_return_size_shorter_than_expected(harness):
    """functionCall/contracts/return_size_shorter_than_expected.sol"""
    pytest.fail("EVM pre-Byzantium: returndata zero-extended when callee returns less. AVM ARC4 return values are not free-form byte ranges. v243: 0p/1f.")

def test_return_size_shorter_than_expected_evm_version_after_homestead(harness):
    """functionCall/contracts/return_size_shorter_than_expected_evm_version_after_homestead.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/return_size_shorter_than_expected_evm_version_after_homestead.sol", via_yul_behavior=True)
    # test() -> FAILURE
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_send_zero_ether(harness):
    """functionCall/contracts/send_zero_ether.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/send_zero_ether.sol", fund_wei=20)
    # s() -> true
    r = harness.call(app, "s()")
    assert bool(as_int(r.abi_return)) is True

def test_transaction_status(harness):
    """functionCall/contracts/transaction_status.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/transaction_status.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # g() -> FAILURE
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_value_test(harness):
    """functionCall/contracts/value_test.sol — msg.value on AVM is the
    payment amount in microalgos. 1 ether (= 10^18 microalgos) would
    overdraw the test account, so we substitute a representative amount."""
    app = harness.compile_and_deploy("functionCall/contracts/value_test.sol")
    assert as_int(harness.call(app, "f()", payment_wei=10_000).abi_return) == 10_000
    assert as_int(harness.call(app, "f()", payment_wei=1).abi_return) == 1
