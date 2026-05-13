"""Auto-generated tests for the enums category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_constructing_enums_from_ints(harness):
    """enums/contracts/constructing_enums_from_ints.sol"""
    app = harness.compile_and_deploy("enums/contracts/constructing_enums_from_ints.sol")
    # test() -> 1
    r = harness.call(app, "test()")
    assert r.abi_return == 1

def test_enum_explicit_overflow(harness):
    """enums/contracts/enum_explicit_overflow.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_explicit_overflow.sol")
    # getChoiceExp(uint256): 2 -> 2
    r = harness.call(app, "getChoiceExp(uint256)", 2)
    assert r.abi_return == 2
    # getChoiceExp(uint256): 3 -> FAILURE, hex"4e487b71", 0x21 # These should throw #
    r = harness.call(app, "getChoiceExp(uint256)", 3, expect_revert=True)
    assert r.reverted
    # getChoiceFromSigned(int256): -1 -> FAILURE, hex"4e487b71", 0x21
    r = harness.call(app, "getChoiceFromSigned(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # getChoiceFromMax() -> FAILURE, hex"4e487b71", 0x21
    r = harness.call(app, "getChoiceFromMax()", expect_revert=True)
    assert r.reverted
    # getChoiceExp(uint256): 2 -> 2 # These should work #
    r = harness.call(app, "getChoiceExp(uint256)", 2)
    # TODO: verify expected: 2 # These should work #
    assert not r.reverted
    # getChoiceExp(uint256): 0 -> 0
    r = harness.call(app, "getChoiceExp(uint256)", 0)
    assert r.abi_return == 0

def test_enum_explicit_overflow_homestead(harness):
    """enums/contracts/enum_explicit_overflow_homestead.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_explicit_overflow_homestead.sol", evm_version='spuriousDragon')
    # getChoiceExp(uint256): 3 -> FAILURE # These should throw #
    r = harness.call(app, "getChoiceExp(uint256)", 3, expect_revert=True)
    assert r.reverted
    # getChoiceFromSigned(int256): -1 -> FAILURE
    r = harness.call(app, "getChoiceFromSigned(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # getChoiceFromMax() -> FAILURE
    r = harness.call(app, "getChoiceFromMax()", expect_revert=True)
    assert r.reverted
    # getChoiceExp(uint256): 2 -> 2 # These should work #
    r = harness.call(app, "getChoiceExp(uint256)", 2)
    # TODO: verify expected: 2 # These should work #
    assert not r.reverted
    # getChoiceExp(uint256): 0 -> 0
    r = harness.call(app, "getChoiceExp(uint256)", 0)
    assert r.abi_return == 0

def test_enum_referencing(harness):
    """enums/contracts/enum_referencing.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_referencing.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3
    # f() -> 3
    r = harness.call(app, "f()")
    assert r.abi_return == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3
    # h() -> 1
    r = harness.call(app, "h()")
    assert r.abi_return == 1
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # y() -> 3
    r = harness.call(app, "y()")
    assert r.abi_return == 3

def test_enum_with_256_members(harness):
    """enums/contracts/enum_with_256_members.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_with_256_members.sol")
    # getMinMax() -> 0, 255
    r = harness.call(app, "getMinMax()")
    assert tuple(r.abi_return) == (0, 255)
    # intToEnum(uint8): 0 -> 0
    r = harness.call(app, "intToEnum(uint8)", 0)
    assert r.abi_return == 0
    # intToEnum(uint8): 255 -> 255
    r = harness.call(app, "intToEnum(uint8)", 255)
    assert r.abi_return == 255
    # enumToInt(uint8): 0 -> 0
    r = harness.call(app, "enumToInt(uint8)", 0)
    assert r.abi_return == 0
    # enumToInt(uint8): 255 -> 255
    r = harness.call(app, "enumToInt(uint8)", 255)
    assert r.abi_return == 255
    # enumToInt(uint8): 256 -> FAILURE
    r = harness.call(app, "enumToInt(uint8)", 256, expect_revert=True)
    assert r.reverted
    # decodeEnum(bytes): 0x20, 32, 0 -> 0
    r = harness.call(app, "decodeEnum(bytes)", 32, 32, 0)
    assert r.abi_return == 0
    # decodeEnum(bytes): 0x20, 32, 255 -> 255
    r = harness.call(app, "decodeEnum(bytes)", 32, 32, 255)
    assert r.abi_return == 255
    # decodeEnum(bytes): 0x20, 32, 256 -> FAILURE
    r = harness.call(app, "decodeEnum(bytes)", 32, 32, 256, expect_revert=True)
    assert r.reverted

def test_invalid_enum_logged(harness):
    """enums/contracts/invalid_enum_logged.sol"""
    app = harness.compile_and_deploy("enums/contracts/invalid_enum_logged.sol")
    # test_log_ok() -> 1
    r = harness.call(app, "test_log_ok()")
    assert r.abi_return == 1
    # test_log() -> FAILURE, hex"4e487b71", 0x21
    r = harness.call(app, "test_log()", expect_revert=True)
    assert r.reverted

def test_minmax(harness):
    """enums/contracts/minmax.sol"""
    app = harness.compile_and_deploy("enums/contracts/minmax.sol")
    # min() -> 0
    r = harness.call(app, "min()")
    assert r.abi_return == 0
    # max() -> 3
    r = harness.call(app, "max()")
    assert r.abi_return == 3

def test_using_contract_enums_with_explicit_contract_name(harness):
    """enums/contracts/using_contract_enums_with_explicit_contract_name.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_contract_enums_with_explicit_contract_name.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert r.abi_return == 1

def test_using_enums(harness):
    """enums/contracts/using_enums.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_enums.sol")
    # getChoice() -> 2
    r = harness.call(app, "getChoice()")
    assert r.abi_return == 2

def test_using_inherited_enum(harness):
    """enums/contracts/using_inherited_enum.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_inherited_enum.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert r.abi_return == 1

def test_using_inherited_enum_excplicitly(harness):
    """enums/contracts/using_inherited_enum_excplicitly.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_inherited_enum_excplicitly.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert r.abi_return == 1
