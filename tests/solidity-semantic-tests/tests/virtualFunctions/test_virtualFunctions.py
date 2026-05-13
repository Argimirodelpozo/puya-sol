"""Tests for the virtualFunctions category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_internal_virtual_function_calls(harness):
    """virtualFunctions/contracts/internal_virtual_function_calls.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/internal_virtual_function_calls.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_internal_virtual_function_calls_through_dispatch(harness):
    """virtualFunctions/contracts/internal_virtual_function_calls_through_dispatch.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/internal_virtual_function_calls_through_dispatch.sol")
    # h() -> 2
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 2

def test_virtual_function_calls(harness):
    """virtualFunctions/contracts/virtual_function_calls.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/virtual_function_calls.sol")
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_virtual_function_usage_in_constructor_arguments(harness):
    """virtualFunctions/contracts/virtual_function_usage_in_constructor_arguments.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/virtual_function_usage_in_constructor_arguments.sol")
    # getA() -> 2
    r = harness.call(app, "getA()")
    assert as_int(r.abi_return) == 2

def test_virtual_override_changing_mutability_internal(harness):
    """virtualFunctions/contracts/virtual_override_changing_mutability_internal.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/virtual_override_changing_mutability_internal.sol")
    # run() ->
    r = harness.call(app, "run()")
    # (void return — call succeeding is the assertion)

def test_virtual_override_changing_mutability_public(harness):
    """virtualFunctions/contracts/virtual_override_changing_mutability_public.sol"""
    app = harness.compile_and_deploy("virtualFunctions/contracts/virtual_override_changing_mutability_public.sol")
    # run() ->
    r = harness.call(app, "run()")
    # (void return — call succeeding is the assertion)
