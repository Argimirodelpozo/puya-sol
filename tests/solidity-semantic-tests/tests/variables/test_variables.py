"""Auto-generated tests for the variables category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_delete_local(harness):
    """variables/delete_local.sol"""
    app = harness.compile_and_deploy("variables/delete_local.sol")
    # delLocal() -> 0
    r = harness.call(app, "delLocal()")
    assert r.abi_return == 0

def test_delete_locals(harness):
    """variables/delete_locals.sol"""
    app = harness.compile_and_deploy("variables/delete_locals.sol")
    # delLocal() -> 6, 7
    r = harness.call(app, "delLocal()")
    assert tuple(r.abi_return) == (6, 7)

def test_delete_transient_state_variable(harness):
    """variables/delete_transient_state_variable.sol"""
    app = harness.compile_and_deploy("variables/delete_transient_state_variable.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_delete_transient_state_variable_non_zero_offset(harness):
    """variables/delete_transient_state_variable_non_zero_offset.sol"""
    app = harness.compile_and_deploy("variables/delete_transient_state_variable_non_zero_offset.sol")
    # f() -> 0xffffffffffffffffffffffffffff000000000000000000000000000000000000, 0, 0xffffffffffffffffffffffffffff
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (115792089237316195423570985008687885552524786135017422503739311359551623659520, 0, 5192296858534827628530496329220095)

def test_mapping_local_assignment(harness):
    """variables/mapping_local_assignment.sol"""
    app = harness.compile_and_deploy("variables/mapping_local_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (42, 0, 0, 21)

def test_mapping_local_compound_assignment(harness):
    """variables/mapping_local_compound_assignment.sol"""
    app = harness.compile_and_deploy("variables/mapping_local_compound_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (42, 0, 0, 21)

def test_mapping_local_tuple_assignment(harness):
    """variables/mapping_local_tuple_assignment.sol"""
    app = harness.compile_and_deploy("variables/mapping_local_tuple_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (42, 0, 0, 21)

def test_public_state_overridding(harness):
    """variables/public_state_overridding.sol"""
    app = harness.compile_and_deploy("variables/public_state_overridding.sol")
    # test() -> 0
    r = harness.call(app, "test()")
    assert r.abi_return == 0
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # test() -> 2
    r = harness.call(app, "test()")
    assert r.abi_return == 2

def test_public_state_overridding_dynamic_struct(harness):
    """variables/public_state_overridding_dynamic_struct.sol"""
    app = harness.compile_and_deploy("variables/public_state_overridding_dynamic_struct.sol")
    # test() -> 0, 64, 0
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (0, 64, 0)
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # test() -> 2, 0x40, 8, "statevar"
    r = harness.call(app, "test()")
    # TODO: verify expected: 2 | 0x40 | 8 | "statevar"
    assert not r.reverted

def test_public_state_overridding_mapping_to_dynamic_struct(harness):
    """variables/public_state_overridding_mapping_to_dynamic_struct.sol"""
    app = harness.compile_and_deploy("variables/public_state_overridding_mapping_to_dynamic_struct.sol")
    # test(uint256): 0 -> 0, 64, 0
    r = harness.call(app, "test(uint256)", 0)
    assert tuple(r.abi_return) == (0, 64, 0)
    # test(uint256): 42 -> 0, 64, 0
    r = harness.call(app, "test(uint256)", 42)
    assert tuple(r.abi_return) == (0, 64, 0)
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # test(uint256): 0 -> 0, 64, 0
    r = harness.call(app, "test(uint256)", 0)
    assert tuple(r.abi_return) == (0, 64, 0)
    # test(uint256): 42 -> 2, 0x40, 8, "statevar"
    r = harness.call(app, "test(uint256)", 42)
    # TODO: verify expected: 2 | 0x40 | 8 | "statevar"
    assert not r.reverted

def test_storing_invalid_boolean(harness):
    """variables/storing_invalid_boolean.sol"""
    app = harness.compile_and_deploy("variables/storing_invalid_boolean.sol")
    # set() -> 1
    r = harness.call(app, "set()")
    assert r.abi_return == 1
    # perm() -> true
    r = harness.call(app, "perm()")
    assert r.abi_return is True
    # ret() -> true
    r = harness.call(app, "ret()")
    assert r.abi_return is True
    # ev() -> 1
    r = harness.call(app, "ev()")
    assert r.abi_return == 1

def test_transient_function_type_state_variable(harness):
    """variables/transient_function_type_state_variable.sol"""
    app = harness.compile_and_deploy("variables/transient_function_type_state_variable.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_transient_state_address_variable_members(harness):
    """variables/transient_state_address_variable_members.sol"""
    app = harness.compile_and_deploy("variables/transient_state_address_variable_members.sol")
    # f() -> 1267650600228229401496703205376
    r = harness.call(app, "f()")
    assert r.abi_return == 1267650600228229401496703205376
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0

def test_transient_state_enum_variable(harness):
    """variables/transient_state_enum_variable.sol"""
    app = harness.compile_and_deploy("variables/transient_state_enum_variable.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_transient_state_variable(harness):
    """variables/transient_state_variable.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0
    # g() -> 8
    r = harness.call(app, "g()")
    assert r.abi_return == 8
    # h() -> 0
    r = harness.call(app, "h()")
    assert r.abi_return == 0

def test_transient_state_variable_cleanup_assignment(harness):
    """variables/transient_state_variable_cleanup_assignment.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_cleanup_assignment.sol")
    # f() -> 0xff
    r = harness.call(app, "f()")
    assert r.abi_return == 255

def test_transient_state_variable_cleanup_tstore(harness):
    """variables/transient_state_variable_cleanup_tstore.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_cleanup_tstore.sol")
    # f() -> 0xff
    r = harness.call(app, "f()")
    assert r.abi_return == 255

def test_transient_state_variable_slot_inline_assembly(harness):
    """variables/transient_state_variable_slot_inline_assembly.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_slot_inline_assembly.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (0, 0)
    # g() -> 1, 0
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (1, 0)
    # h() -> 2, 0
    r = harness.call(app, "h()")
    assert tuple(r.abi_return) == (2, 0)

def test_transient_state_variable_slots_and_offsets(harness):
    """variables/transient_state_variable_slots_and_offsets.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_slots_and_offsets.sol")
    # f() -> 1, 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2, 3, 4)

def test_transient_state_variable_tuple_assignment(harness):
    """variables/transient_state_variable_tuple_assignment.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_tuple_assignment.sol")
    # f() -> 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (2, 3, 4)

def test_transient_state_variable_udvt(harness):
    """variables/transient_state_variable_udvt.sol"""
    app = harness.compile_and_deploy("variables/transient_state_variable_udvt.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2
    # h() -> 0
    r = harness.call(app, "h()")
    assert r.abi_return == 0
