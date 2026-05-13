"""Auto-generated tests for the constants category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_asm_address_constant_regression(harness):
    """constants/contracts/asm_address_constant_regression.sol"""
    app = harness.compile_and_deploy("constants/contracts/asm_address_constant_regression.sol")
    # f() -> 0x00
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_asm_constant_file_level(harness):
    """constants/contracts/asm_constant_file_level.sol"""
    app = harness.compile_and_deploy("constants/contracts/asm_constant_file_level.sol")
    # f() -> 0x1212121212121212121212121000002134593163
    r = harness.call(app, "f()")
    assert r.abi_return == 103164821458651970696730694073941364629493592419

def test_assign_type_info(harness):
    """constants/contracts/assign_type_info.sol"""
    app = harness.compile_and_deploy("constants/contracts/assign_type_info.sol")
    # nonEmptyCode() -> true
    r = harness.call(app, "nonEmptyCode()")
    assert r.abi_return is True

def test_constant_string(harness):
    """constants/contracts/constant_string.sol"""
    app = harness.compile_and_deploy("constants/contracts/constant_string.sol")
    # f() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "f()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # g() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "g()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # h() -> 0x20, 5, "hello"
    r = harness.call(app, "h()")
    assert r.abi_return == 'hello'

def test_constant_string_at_file_level(harness):
    """constants/contracts/constant_string_at_file_level.sol"""
    app = harness.compile_and_deploy("constants/contracts/constant_string_at_file_level.sol")
    # f() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "f()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # g() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "g()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # h() -> 0x20, 5, "hello"
    r = harness.call(app, "h()")
    assert r.abi_return == 'hello'
    # i() -> 0x38, 1, 0x61626300ff5f5f00000000000000000000000000000000000000000000000000
    r = harness.call(app, "i()")
    assert tuple(r.abi_return) == (56, 1, 44048180624707321370159228589897778088919435935156254407473833945046349512704)

def test_constant_variables(harness):
    """constants/contracts/constant_variables.sol"""
    app = harness.compile_and_deploy("constants/contracts/constant_variables.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_constants_at_file_level_referencing(harness):
    """constants/contracts/constants_at_file_level_referencing.sol"""
    app = harness.compile_and_deploy("constants/contracts/constants_at_file_level_referencing.sol")
    # f() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "f()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # g() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "g()")
    assert r.abi_return == '\\x03\\x01\\x02'
    # h() -> 5
    r = harness.call(app, "h()")
    assert r.abi_return == 5
    # i() -> 0x20, 3, "\x03\x01\x02"
    r = harness.call(app, "i()")
    assert r.abi_return == '\\x03\\x01\\x02'

def test_consteval_array_length(harness):
    """constants/contracts/consteval_array_length.sol"""
    app = harness.compile_and_deploy("constants/contracts/consteval_array_length.sol", via_yul_behavior=True)
    # f() -> 0x0a, 0x0a
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (10, 10)

def test_function_unreferenced(harness):
    """constants/contracts/function_unreferenced.sol"""
    app = harness.compile_and_deploy("constants/contracts/function_unreferenced.sol")
    # f() -> 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 102264414861304285884729579275374176073311626045629144087797787832582884294656

def test_same_constants_different_files(harness):
    """constants/contracts/same_constants_different_files.sol"""
    app = harness.compile_and_deploy("constants/contracts/same_constants_different_files.sol")
    # f() -> 0x0d, 0x59, 0x59, 0x59
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (13, 89, 89, 89)

def test_simple_constant_variables_test(harness):
    """constants/contracts/simple_constant_variables_test.sol"""
    app = harness.compile_and_deploy("constants/contracts/simple_constant_variables_test.sol")
    # getX() -> 56
    r = harness.call(app, "getX()")
    assert r.abi_return == 56
