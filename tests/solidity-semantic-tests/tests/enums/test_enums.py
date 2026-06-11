"""Tests for the enums category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_constructing_enums_from_ints(harness):
    """enums/contracts/constructing_enums_from_ints.sol"""
    app = harness.compile_and_deploy("enums/contracts/constructing_enums_from_ints.sol")
    # test() -> 1
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 1

def test_enum_explicit_overflow(harness):
    """enums/contracts/enum_explicit_overflow.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_explicit_overflow.sol")
    # getChoiceExp(uint256): 2 -> 2
    r = harness.call(app, "getChoiceExp(uint256)", 2)
    assert as_int(r.abi_return) == 2
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
    assert as_int(r.abi_return) == 0

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
    assert as_int(r.abi_return) == 0

def test_enum_referencing(harness):
    """enums/contracts/enum_referencing.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_referencing.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3
    # h() -> 1
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 1
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # y() -> 3
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 3

def test_enum_with_256_members(harness):
    """enums/contracts/enum_with_256_members.sol"""
    app = harness.compile_and_deploy("enums/contracts/enum_with_256_members.sol")
    # getMinMax() -> 0, 255
    r = harness.call(app, "getMinMax()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 255)
    # intToEnum(uint8): 0 -> 0
    r = harness.call(app, "intToEnum(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # intToEnum(uint8): 255 -> 255
    r = harness.call(app, "intToEnum(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # enumToInt(uint8): 0 -> 0
    r = harness.call(app, "enumToInt(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # enumToInt(uint8): 255 -> 255
    r = harness.call(app, "enumToInt(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # enumToInt(uint8): 256 -> FAILURE
    r = harness.call(app, "enumToInt(uint8)", 256, expect_revert=True)
    assert r.reverted
    # decodeEnum(bytes) takes a 32-byte payload encoding a single uint and
    # decodes it as the enum.
    assert as_int(harness.call(app, "decodeEnum(bytes)", (0).to_bytes(32, "big")).abi_return) == 0
    assert as_int(harness.call(app, "decodeEnum(bytes)", (255).to_bytes(32, "big")).abi_return) == 255
    assert harness.call(app, "decodeEnum(bytes)", (256).to_bytes(32, "big"), expect_revert=True).reverted

def test_invalid_enum_logged(harness):
    """enums/contracts/invalid_enum_logged.sol"""
    app = harness.compile_and_deploy("enums/contracts/invalid_enum_logged.sol")
    # test_log_ok() -> 1
    r = harness.call(app, "test_log_ok()")
    assert as_int(r.abi_return) == 1
    # test_log() -> FAILURE, hex"4e487b71", 0x21
    r = harness.call(app, "test_log()", expect_revert=True)
    assert r.reverted

def test_minmax(harness):
    """enums/contracts/minmax.sol"""
    app = harness.compile_and_deploy("enums/contracts/minmax.sol")
    # min() -> 0
    r = harness.call(app, "min()")
    assert as_int(r.abi_return) == 0
    # max() -> 3
    r = harness.call(app, "max()")
    assert as_int(r.abi_return) == 3

def test_using_contract_enums_with_explicit_contract_name(harness):
    """enums/contracts/using_contract_enums_with_explicit_contract_name.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_contract_enums_with_explicit_contract_name.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert as_int(r.abi_return) == 1

def test_using_enums(harness):
    """enums/contracts/using_enums.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_enums.sol")
    # getChoice() -> 2
    r = harness.call(app, "getChoice()")
    assert as_int(r.abi_return) == 2

def test_using_inherited_enum(harness):
    """enums/contracts/using_inherited_enum.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_inherited_enum.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert as_int(r.abi_return) == 1

def test_using_inherited_enum_excplicitly(harness):
    """enums/contracts/using_inherited_enum_excplicitly.sol"""
    app = harness.compile_and_deploy("enums/contracts/using_inherited_enum_excplicitly.sol")
    # answer() -> 1
    r = harness.call(app, "answer()")
    assert as_int(r.abi_return) == 1


def test_enum_side_effect_once(harness):
    """enums/contracts/enum_side_effect_once.sol

    CUSTOM regression guard (NOT vendored). A side-effecting enum value used in
    an `emit E(f())` or a direct `return f()` must evaluate once. Both sites
    reference the value twice (range-assert 0x21 + the field/return) and ran
    f() twice before the fix (makeEvalOnce wrap in SolEmitStatement /
    SolExpressionStatement).
    """
    app = harness.compile_and_deploy("enums/contracts/enum_side_effect_once.sol")
    assert as_int(harness.call(app, "emitOnce()").abi_return) == 1
    assert tuple(as_int(x) for x in harness.call(app, "retOnce()").abi_return) == (1, 1)
    harness.call(app, "retDirect()")
    assert as_int(harness.call(app, "cnt()").abi_return) == 1
