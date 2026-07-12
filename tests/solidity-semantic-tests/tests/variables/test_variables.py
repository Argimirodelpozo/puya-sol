"""Tests for the variables category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes, as_signed_int,
)


def test_transient_int128_signextend(harness):
    """variables/contracts/transient_int128_signext_check.sol

    CUSTOM regression guard (NOT vendored from the upstream Solidity semantic
    suite). A signed sub-256 transient variable (int128) is stored as its raw
    N-bit two's complement and must be sign-extended to canonical 256-bit on
    read, so it compares/arithmetics equal to a scalar of the same value
    (before the fix a read of -5 yielded 2^128-5).
    """
    app = harness.compile_and_deploy("variables/contracts/transient_int128_signext_check.sol")
    # write-then-read round-trip within one call, compared to the scalar
    assert harness.call(app, "roundtrip(int128)", -5).abi_return is True
    assert harness.call(app, "roundtrip(int128)", 777).abi_return is True
    assert harness.call(app, "roundtrip(int128)", -(2 ** 126)).abi_return is True
    # the read sign-extends, so widening to int256 preserves the sign
    assert as_signed_int(harness.call(app, "widen(int128)", -5).abi_return) == -5
    assert as_signed_int(harness.call(app, "widen(int128)", -(2 ** 126)).abi_return) == -(2 ** 126)


def test_delete_local(harness):
    """variables/contracts/delete_local.sol"""
    app = harness.compile_and_deploy("variables/contracts/delete_local.sol")
    # delLocal() -> 0
    r = harness.call(app, "delLocal()")
    assert as_int(r.abi_return) == 0

def test_delete_locals(harness):
    """variables/contracts/delete_locals.sol"""
    app = harness.compile_and_deploy("variables/contracts/delete_locals.sol")
    # delLocal() -> 6, 7
    r = harness.call(app, "delLocal()")
    assert tuple(as_int(x) for x in r.abi_return) == (6, 7)

def test_delete_transient_state_variable(harness):
    """variables/contracts/delete_transient_state_variable.sol"""
    app = harness.compile_and_deploy("variables/contracts/delete_transient_state_variable.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_delete_transient_state_variable_non_zero_offset(harness):
    """variables/contracts/delete_transient_state_variable_non_zero_offset.sol"""
    app = harness.compile_and_deploy('variables/contracts/delete_transient_state_variable_non_zero_offset.sol')
    r = harness.call(app, 'f()')
    # ARC-4 bytes14 is byte[14] (raw bytes), not solc's left-aligned 32-byte word;
    # the delete-y-at-nonzero-offset packed-slot semantics are what's under test.
    assert as_bytes(r.abi_return[0]) == bytes.fromhex('ff' * 14)
    assert as_int(r.abi_return[1]) == 0
    assert as_int(r.abi_return[2]) == 0xffffffffffffffffffffffffffff

def test_mapping_local_assignment(harness):
    """variables/contracts/mapping_local_assignment.sol"""
    app = harness.compile_and_deploy("variables/contracts/mapping_local_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0, 0, 21)

def test_mapping_local_compound_assignment(harness):
    """variables/contracts/mapping_local_compound_assignment.sol"""
    app = harness.compile_and_deploy("variables/contracts/mapping_local_compound_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0, 0, 21)

def test_mapping_local_tuple_assignment(harness):
    """variables/contracts/mapping_local_tuple_assignment.sol"""
    app = harness.compile_and_deploy("variables/contracts/mapping_local_tuple_assignment.sol")
    # f() -> 42, 0, 0, 21
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0, 0, 21)

def test_public_state_overridding(harness):
    """variables/contracts/public_state_overridding.sol"""
    app = harness.compile_and_deploy("variables/contracts/public_state_overridding.sol")
    # test() -> 0
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 0
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # test() -> 2
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 2

def test_public_state_overridding_dynamic_struct(harness):
    """variables/contracts/public_state_overridding_dynamic_struct.sol"""
    app = harness.compile_and_deploy("variables/contracts/public_state_overridding_dynamic_struct.sol")
    # test() returns (v, s) — the auto-generated struct getter.
    r = harness.call(app, "test()")
    assert as_int(r.abi_return[0]) == 0
    assert r.abi_return[1] == ""
    harness.call(app, "set()")
    r = harness.call(app, "test()")
    assert as_int(r.abi_return[0]) == 2
    assert r.abi_return[1] == "statevar"

def test_public_state_overridding_mapping_to_dynamic_struct(harness):
    """variables/contracts/public_state_overridding_mapping_to_dynamic_struct.sol"""
    app = harness.compile_and_deploy("variables/contracts/public_state_overridding_mapping_to_dynamic_struct.sol")
    # test(uint256) returns (v, s) — mapping(uint256 => S) auto-getter.
    r = harness.call(app, "test(uint256)", 0)
    assert as_int(r.abi_return[0]) == 0
    assert r.abi_return[1] == ""
    r = harness.call(app, "test(uint256)", 42)
    assert as_int(r.abi_return[0]) == 0
    assert r.abi_return[1] == ""
    harness.call(app, "set()")
    r = harness.call(app, "test(uint256)", 0)
    assert as_int(r.abi_return[0]) == 0
    assert r.abi_return[1] == ""
    r = harness.call(app, "test(uint256)", 42)
    assert as_int(r.abi_return[0]) == 2
    assert r.abi_return[1] == "statevar"

def test_storing_invalid_boolean(harness):
    """variables/contracts/storing_invalid_boolean.sol"""
    app = harness.compile_and_deploy("variables/contracts/storing_invalid_boolean.sol")
    # set() -> 1
    r = harness.call(app, "set()")
    assert as_int(r.abi_return) == 1
    # perm() -> true
    r = harness.call(app, "perm()")
    assert bool(as_int(r.abi_return)) is True
    # ret() -> true
    r = harness.call(app, "ret()")
    assert bool(as_int(r.abi_return)) is True
    # ev() -> 1
    r = harness.call(app, "ev()")
    assert as_int(r.abi_return) == 1

def test_transient_function_type_state_variable(harness):
    """variables/contracts/transient_function_type_state_variable.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_function_type_state_variable.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_transient_state_address_variable_members(harness):
    """variables/contracts/transient_state_address_variable_members.sol"""
    app = harness.compile_and_deploy('variables/contracts/transient_state_address_variable_members.sol')

def test_transient_state_enum_variable(harness):
    """variables/contracts/transient_state_enum_variable.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_enum_variable.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_transient_state_variable(harness):
    """variables/contracts/transient_state_variable.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # g() -> 8
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 8
    # h() -> 0
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 0

def test_transient_state_variable_cleanup_assignment(harness):
    """variables/contracts/transient_state_variable_cleanup_assignment.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_cleanup_assignment.sol")
    # f() -> 0xff
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 255

def test_transient_state_variable_cleanup_tstore(harness):
    """variables/contracts/transient_state_variable_cleanup_tstore.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_cleanup_tstore.sol")
    # f() -> 0xff
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 255

def test_transient_state_variable_slot_inline_assembly(harness):
    """variables/contracts/transient_state_variable_slot_inline_assembly.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_slot_inline_assembly.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # g() -> 1, 0
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)
    # h() -> 2, 0
    r = harness.call(app, "h()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 0)

def test_transient_state_variable_slots_and_offsets(harness):
    """variables/contracts/transient_state_variable_slots_and_offsets.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_slots_and_offsets.sol")
    # f() -> 1, 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 4)

def test_transient_state_variable_tuple_assignment(harness):
    """variables/contracts/transient_state_variable_tuple_assignment.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_tuple_assignment.sol")
    # f() -> 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 3, 4)

def test_transient_state_variable_udvt(harness):
    """variables/contracts/transient_state_variable_udvt.sol"""
    app = harness.compile_and_deploy("variables/contracts/transient_state_variable_udvt.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # h() -> 0
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 0


def test_tuple_destructure_shadow(harness):
    """variables/contracts/tuple_destructure_shadow.sol

    CUSTOM regression guard (NOT vendored). A tuple-destructured local that
    shadows an outer variable must get a shadow-safe unique name; before the fix
    the inner destructured `a` overwrote the outer `a` (shadowTuple returned 1,
    not 100). The single-decl path was already correct.
    """
    app = harness.compile_and_deploy("variables/contracts/tuple_destructure_shadow.sol")
    assert as_int(harness.call(app, "shadowTuple()").abi_return) == 100
    assert as_int(harness.call(app, "shadowSingle()").abi_return) == 100
