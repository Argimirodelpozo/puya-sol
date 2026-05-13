"""Auto-generated tests for the functionCall category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_array_multiple_local_vars(harness):
    """functionCall/contracts/array_multiple_local_vars.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/array_multiple_local_vars.sol")
    # f(uint256[]): 32, 3, 1000, 1, 2 -> 3
    r = harness.call(app, "f(uint256[])", 32, 3, 1000, 1, 2)
    assert r.abi_return == 3
    # f(uint256[]): 32, 3, 100, 500, 300 -> 600
    r = harness.call(app, "f(uint256[])", 32, 3, 100, 500, 300)
    assert r.abi_return == 600
    # f(uint256[]): 32, 11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 111 -> 55
    r = harness.call(app, "f(uint256[])", 32, 11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 111)
    assert r.abi_return == 55

def test_bare_call_no_returndatacopy(harness):
    """functionCall/contracts/bare_call_no_returndatacopy.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/bare_call_no_returndatacopy.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_call_attached_library_function_on_function(harness):
    """functionCall/contracts/call_attached_library_function_on_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_function.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7

def test_call_attached_library_function_on_storage_variable(harness):
    """functionCall/contracts/call_attached_library_function_on_storage_variable.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_storage_variable.sol")
    # f(uint256): 7 -> 0x2a
    r = harness.call(app, "f(uint256)", 7)
    assert r.abi_return == 42
    # x() -> 0x2a
    r = harness.call(app, "x()")
    assert r.abi_return == 42

def test_call_attached_library_function_on_string(harness):
    """functionCall/contracts/call_attached_library_function_on_string.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_attached_library_function_on_string.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3

def test_call_function_returning_function(harness):
    """functionCall/contracts/call_function_returning_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_function_returning_function.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_call_function_returning_nothing_via_pointer(harness):
    """functionCall/contracts/call_function_returning_nothing_via_pointer.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_function_returning_nothing_via_pointer.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # flag() -> true
    r = harness.call(app, "flag()")
    assert r.abi_return is True

def test_call_internal_function_via_expression(harness):
    """functionCall/contracts/call_internal_function_via_expression.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_internal_function_via_expression.sol")
    # associated() -> 42
    r = harness.call(app, "associated()")
    assert r.abi_return == 42
    # unassociated() -> 42
    r = harness.call(app, "unassociated()")
    assert r.abi_return == 42

def test_call_internal_function_with_multislot_arguments_via_pointer(harness):
    """functionCall/contracts/call_internal_function_with_multislot_arguments_via_pointer.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_internal_function_with_multislot_arguments_via_pointer.sol")
    # test() -> 12
    r = harness.call(app, "test()")
    assert r.abi_return == 12

def test_call_options_overload(harness):
    """functionCall/contracts/call_options_overload.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/call_options_overload.sol")
    # (), 1 ether
    pytest.xfail("fallback() dispatch not yet implemented")
    # call() -> 1, 2, 2, 2
    r = harness.call(app, "call()")
    assert tuple(r.abi_return) == (1, 2, 2, 2)
    # bal() -> 1000000000000000000
    r = harness.call(app, "bal()")
    assert r.abi_return == 1000000000000000000

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
    assert r.abi_return == 7

def test_calling_other_functions(harness):
    """functionCall/contracts/calling_other_functions.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/calling_other_functions.sol")
    # run(uint256): 0 -> 0
    r = harness.call(app, "run(uint256)", 0)
    assert r.abi_return == 0
    # run(uint256): 1 -> 1
    r = harness.call(app, "run(uint256)", 1)
    assert r.abi_return == 1
    # run(uint256): 2 -> 1
    r = harness.call(app, "run(uint256)", 2)
    assert r.abi_return == 1
    # run(uint256): 8 -> 1
    r = harness.call(app, "run(uint256)", 8)
    assert r.abi_return == 1
    # run(uint256): 127 -> 1
    r = harness.call(app, "run(uint256)", 127)
    assert r.abi_return == 1

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
    assert r.abi_return == 1

def test_creation_function_call_no_args(harness):
    """functionCall/contracts/creation_function_call_no_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_no_args.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_creation_function_call_with_args(harness):
    """functionCall/contracts/creation_function_call_with_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_with_args.sol", ctor_args=[2])
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_creation_function_call_with_salt(harness):
    """functionCall/contracts/creation_function_call_with_salt.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/creation_function_call_with_salt.sol", ctor_args=[2])
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_delegatecall_return_value(harness):
    """functionCall/contracts/delegatecall_return_value.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/delegatecall_return_value.sol")
    # get() -> 0x00
    r = harness.call(app, "get()")
    assert r.abi_return == 0
    # assert0_delegated() -> 0x01, 0x40, 0x0
    r = harness.call(app, "assert0_delegated()")
    assert tuple(r.abi_return) == (1, 64, 0)
    # get_delegated() -> 0x01, 0x40, 0x20, 0x0
    r = harness.call(app, "get_delegated()")
    assert tuple(r.abi_return) == (1, 64, 32, 0)
    # set(uint256): 0x01 ->
    r = harness.call(app, "set(uint256)", 1)
    # (void return — call succeeding is the assertion)
    # get() -> 0x01
    r = harness.call(app, "get()")
    assert r.abi_return == 1
    # assert0_delegated() -> 0x00, 0x40, 0x24, 0x4e487b7100000000000000000000000000000000000000000000000000000000, 0x0100000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "assert0_delegated()")
    # TODO: verify structural decoding matches expected: 0, 64, 36, 35408467139433450592217433187231851964531694900788300625387963629091585785856, 26959946667150639794667015087019630673637144422540572481103610249216
    assert not r.reverted
    # get_delegated() -> 0x01, 0x40, 0x20, 0x1
    r = harness.call(app, "get_delegated()")
    assert tuple(r.abi_return) == (1, 64, 32, 1)
    # set(uint256): 0x2a ->
    r = harness.call(app, "set(uint256)", 42)
    # (void return — call succeeding is the assertion)
    # get() -> 0x2a
    r = harness.call(app, "get()")
    assert r.abi_return == 42
    # assert0_delegated() -> 0x00, 0x40, 0x24, 0x4e487b7100000000000000000000000000000000000000000000000000000000, 0x0100000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "assert0_delegated()")
    # TODO: verify structural decoding matches expected: 0, 64, 36, 35408467139433450592217433187231851964531694900788300625387963629091585785856, 26959946667150639794667015087019630673637144422540572481103610249216
    assert not r.reverted
    # get_delegated() -> 0x01, 0x40, 0x20, 0x2a
    r = harness.call(app, "get_delegated()")
    assert tuple(r.abi_return) == (1, 64, 32, 42)

def test_delegatecall_return_value_pre_byzantium(harness):
    """functionCall/contracts/delegatecall_return_value_pre_byzantium.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/delegatecall_return_value_pre_byzantium.sol", evm_version='spuriousDragon')
    # get() -> 0x00
    r = harness.call(app, "get()")
    assert r.abi_return == 0
    # assert0_delegated() -> true
    r = harness.call(app, "assert0_delegated()")
    assert r.abi_return is True
    # get_delegated() -> true
    r = harness.call(app, "get_delegated()")
    assert r.abi_return is True
    # set(uint256): 0x01 ->
    r = harness.call(app, "set(uint256)", 1)
    # (void return — call succeeding is the assertion)
    # get() -> 0x01
    r = harness.call(app, "get()")
    assert r.abi_return == 1
    # assert0_delegated() -> false
    r = harness.call(app, "assert0_delegated()")
    assert r.abi_return is False
    # get_delegated() -> true
    r = harness.call(app, "get_delegated()")
    assert r.abi_return is True
    # set(uint256): 0x2a ->
    r = harness.call(app, "set(uint256)", 42)
    # (void return — call succeeding is the assertion)
    # get() -> 0x2a
    r = harness.call(app, "get()")
    assert r.abi_return == 42
    # assert0_delegated() -> false
    r = harness.call(app, "assert0_delegated()")
    assert r.abi_return is False
    # get_delegated() -> true
    r = harness.call(app, "get_delegated()")
    assert r.abi_return is True

def test_disordered_named_args(harness):
    """functionCall/contracts/disordered_named_args.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/disordered_named_args.sol")
    # b() -> 123
    r = harness.call(app, "b()")
    assert r.abi_return == 123

def test_external_call(harness):
    """functionCall/contracts/external_call.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call.sol")
    # g(uint256): 4 -> 5
    r = harness.call(app, "g(uint256)", 4)
    assert r.abi_return == 5
    # f(uint256): 2 -> 5
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 5

def test_external_call_at_construction_time(harness):
    """functionCall/contracts/external_call_at_construction_time.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_at_construction_time.sol")
    # f(uint256): 0 -> FAILURE
    r = harness.call(app, "f(uint256)", 0, expect_revert=True)
    assert r.reverted
    # f(uint256): 1 -> FAILURE
    r = harness.call(app, "f(uint256)", 1, expect_revert=True)
    assert r.reverted
    # f(uint256): 2 -> 3
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 3

def test_external_call_dynamic_returndata(harness):
    """functionCall/contracts/external_call_dynamic_returndata.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_dynamic_returndata.sol")
    # dt(uint256): 4 -> 6
    r = harness.call(app, "dt(uint256)", 4)
    assert r.abi_return == 6

def test_external_call_to_nonexisting(harness):
    """functionCall/contracts/external_call_to_nonexisting.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_to_nonexisting.sol", fund_wei=1000000000000000000)
    # f(uint256): 0 -> FAILURE
    r = harness.call(app, "f(uint256)", 0, expect_revert=True)
    assert r.reverted
    # f(uint256): 1 -> FAILURE
    r = harness.call(app, "f(uint256)", 1, expect_revert=True)
    assert r.reverted
    # f(uint256): 2 -> FAILURE
    r = harness.call(app, "f(uint256)", 2, expect_revert=True)
    assert r.reverted
    # f(uint256): 3 -> FAILURE
    r = harness.call(app, "f(uint256)", 3, expect_revert=True)
    assert r.reverted
    # f(uint256): 4 -> FAILURE
    r = harness.call(app, "f(uint256)", 4, expect_revert=True)
    assert r.reverted
    # f(uint256): 5 -> FAILURE
    r = harness.call(app, "f(uint256)", 5, expect_revert=True)
    assert r.reverted
    # f(uint256): 6 -> 7
    r = harness.call(app, "f(uint256)", 6)
    assert r.abi_return == 7

def test_external_call_to_nonexisting_debugstrings(harness):
    """functionCall/contracts/external_call_to_nonexisting_debugstrings.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_to_nonexisting_debugstrings.sol", fund_wei=1000000000000000000)
    # f(uint256): 0 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 0, expect_revert=True)
    assert r.reverted
    # f(uint256): 1 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 1, expect_revert=True)
    assert r.reverted
    # f(uint256): 2 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 2, expect_revert=True)
    assert r.reverted
    # f(uint256): 3 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 3, expect_revert=True)
    assert r.reverted
    # f(uint256): 4 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 4, expect_revert=True)
    assert r.reverted
    # f(uint256): 5 -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "f(uint256)", 5, expect_revert=True)
    assert r.reverted
    # f(uint256): 6 -> 7
    r = harness.call(app, "f(uint256)", 6)
    assert r.abi_return == 7

def test_external_call_value(harness):
    """functionCall/contracts/external_call_value.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_call_value.sol")
    # g(uint256), 1 ether: 4 -> 1000000000000000000000, 4
    r = harness.call(app, "g(uint256)", 4, payment_wei=1000000000000000000)
    assert tuple(r.abi_return) == (1000000000000000000000, 4)
    # f(uint256), 11 ether: 2 -> 10000, 2
    r = harness.call(app, "f(uint256)", 2, payment_wei=11000000000000000000)
    assert tuple(r.abi_return) == (10000, 2)

def test_external_function(harness):
    """functionCall/contracts/external_function.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_function.sol")
    # test(uint256,uint256): 2, 3 -> 9, 3
    r = harness.call(app, "test(uint256,uint256)", 2, 3)
    assert tuple(r.abi_return) == (9, 3)

def test_external_public_override(harness):
    """functionCall/contracts/external_public_override.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/external_public_override.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2

def test_failed_create(harness):
    """functionCall/contracts/failed_create.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/failed_create.sol")
    # f(uint256): 20 ->
    r = harness.call(app, "f(uint256)", 20)
    # (void return — call succeeding is the assertion)
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # f(uint256): 20 -> FAILURE
    r = harness.call(app, "f(uint256)", 20, expect_revert=True)
    assert r.reverted
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # stack(uint256): 1023 -> FAILURE
    r = harness.call(app, "stack(uint256)", 1023, expect_revert=True)
    assert r.reverted
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # stack(uint256): 10 ->
    r = harness.call(app, "stack(uint256)", 10)
    # (void return — call succeeding is the assertion)
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2

def test_file_level_call_via_module(harness):
    """functionCall/contracts/file_level_call_via_module.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/file_level_call_via_module.sol")
    # f() -> 7, 3
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (7, 3)

def test_gas_and_value_basic(harness):
    """functionCall/contracts/gas_and_value_basic.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/gas_and_value_basic.sol", fund_wei=20)
    # sendAmount(uint256): 5 -> 5
    r = harness.call(app, "sendAmount(uint256)", 5)
    assert r.abi_return == 5
    # outOfGas() -> FAILURE # call to helper should not succeed but amount should be transferred anyway #
    r = harness.call(app, "outOfGas()", expect_revert=True)
    assert r.reverted
    # checkState() -> false, 15
    r = harness.call(app, "checkState()")
    # TODO: verify expected: false | 15
    assert not r.reverted

def test_mapping_array_internal_argument(harness):
    """functionCall/contracts/mapping_array_internal_argument.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/mapping_array_internal_argument.sol")
    # set(uint8,uint8,uint8,uint8,uint8): 1, 21, 22, 42, 43 -> 0, 0, 0, 0
    r = harness.call(app, "set(uint8,uint8,uint8,uint8,uint8)", 1, 21, 22, 42, 43)
    assert tuple(r.abi_return) == (0, 0, 0, 0)
    # get(uint8): 1 -> 21, 22, 42, 43
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(r.abi_return) == (21, 22, 42, 43)
    # set(uint8,uint8,uint8,uint8,uint8): 1, 10, 30, 11, 31 -> 21, 22, 42, 43
    r = harness.call(app, "set(uint8,uint8,uint8,uint8,uint8)", 1, 10, 30, 11, 31)
    assert tuple(r.abi_return) == (21, 22, 42, 43)
    # get(uint8): 1 -> 10, 30, 11, 31
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(r.abi_return) == (10, 30, 11, 31)

def test_mapping_internal_argument(harness):
    """functionCall/contracts/mapping_internal_argument.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/mapping_internal_argument.sol")
    # set(uint8,uint8,uint8): 1, 21, 42 -> 0, 0
    r = harness.call(app, "set(uint8,uint8,uint8)", 1, 21, 42)
    assert tuple(r.abi_return) == (0, 0)
    # get(uint8): 1 -> 21, 42
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(r.abi_return) == (21, 42)
    # set(uint8,uint8,uint8): 1, 10, 11 -> 21, 42
    r = harness.call(app, "set(uint8,uint8,uint8)", 1, 10, 11)
    assert tuple(r.abi_return) == (21, 42)
    # get(uint8): 1 -> 10, 11
    r = harness.call(app, "get(uint8)", 1)
    assert tuple(r.abi_return) == (10, 11)

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
    assert r.abi_return == 8
    # name() -> "Celina"
    r = harness.call(app, "name()")
    # TODO: verify expected: "Celina"
    assert not r.reverted
    # a_hash() -> 0xa91eddf639b0b768929589c1a9fd21dcb0107199bdd82e55c5348018a1572f52
    r = harness.call(app, "a_hash()")
    assert r.abi_return == 76495408746680389550338499638379293609691976073007980668405434357245214666578
    # an_address() -> 0x1337
    r = harness.call(app, "an_address()")
    assert r.abi_return == 4919
    # super_secret_data() -> FAILURE
    r = harness.call(app, "super_secret_data()", expect_revert=True)
    assert r.reverted

def test_multiple_functions(harness):
    """functionCall/contracts/multiple_functions.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/multiple_functions.sol")
    # a() -> 0
    r = harness.call(app, "a()")
    assert r.abi_return == 0
    # b() -> 1
    r = harness.call(app, "b()")
    assert r.abi_return == 1
    # c() -> 2
    r = harness.call(app, "c()")
    assert r.abi_return == 2
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
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
    assert r.abi_return == 123
    # c() -> 123
    r = harness.call(app, "c()")
    assert r.abi_return == 123

def test_named_args_overload(harness):
    """functionCall/contracts/named_args_overload.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/named_args_overload.sol")
    # call(uint256): 0 -> 0
    r = harness.call(app, "call(uint256)", 0)
    assert r.abi_return == 0
    # call(uint256): 1 -> 1
    r = harness.call(app, "call(uint256)", 1)
    assert r.abi_return == 1
    # call(uint256): 2 -> 3
    r = harness.call(app, "call(uint256)", 2)
    assert r.abi_return == 3
    # call(uint256): 3 -> 6
    r = harness.call(app, "call(uint256)", 3)
    assert r.abi_return == 6
    # call(uint256): 4 -> 8
    r = harness.call(app, "call(uint256)", 4)
    assert r.abi_return == 8
    # call(uint256): 5 -> 500
    r = harness.call(app, "call(uint256)", 5)
    assert r.abi_return == 500

def test_precompile_extcodesize_check(harness):
    """functionCall/contracts/precompile_extcodesize_check.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/precompile_extcodesize_check.sol")
    # testHighLevel() -> true
    r = harness.call(app, "testHighLevel()")
    assert r.abi_return is True
    # testLowLevel() -> 0xc76596d400000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "testLowLevel()")
    assert r.abi_return == 90189749399073340124083172608208815642393485993436412853902028917580719194112
    # testHighLevel2() -> FAILURE
    r = harness.call(app, "testHighLevel2()", expect_revert=True)
    assert r.reverted

def test_return_size_bigger_than_expected(harness):
    """functionCall/contracts/return_size_bigger_than_expected.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/return_size_bigger_than_expected.sol", via_yul_behavior=True)
    # test() -> 0x20
    r = harness.call(app, "test()")
    assert r.abi_return == 32

def test_return_size_shorter_than_expected(harness):
    """functionCall/contracts/return_size_shorter_than_expected.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/return_size_shorter_than_expected.sol", via_yul_behavior=True, evm_version='homestead')
    # test() -> 0x0500
    r = harness.call(app, "test()")
    assert r.abi_return == 1280

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
    assert r.abi_return is True

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
    """functionCall/contracts/value_test.sol"""
    app = harness.compile_and_deploy("functionCall/contracts/value_test.sol")
    # f(), 1 ether -> 1000000000000000000
    r = harness.call(app, "f()", payment_wei=1000000000000000000)
    assert r.abi_return == 1000000000000000000
    # f(), 1 wei -> 1
    r = harness.call(app, "f()", payment_wei=1)
    assert r.abi_return == 1
