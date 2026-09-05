"""Regression coverage for the September 2026 builder audit.

These are puya-sol-specific correctness guards, not vendored Solidity tests.
"""

import json

import pytest

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True])
def test_operator_call_plan(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/operator_call_plan.sol", via_yul_behavior=via_ir)
    for value in (0, 3, 17):
        result = harness.call(app, "unary(uint64)", value).abi_return
        assert tuple(map(as_int, result)) == (value + 7, value + 11)
    assert as_int(harness.call(app, "binary(uint64,uint64)", 3, 8).abi_return) == 38
    result = harness.call(app, "sequenced()").abi_return
    assert tuple(map(as_int, result)) == (12, 2)


@pytest.mark.parametrize("via_ir", [False, True])
def test_full_word_bool_cleanup(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/bool_word_cleanup.sol", via_yul_behavior=via_ir)
    for word in (0, 1, 2, 1 << 64, 1 << 128, 1 << 255):
        assert harness.call(app, "clean(uint256)", word).abi_return is bool(word)
        if word < 2:
            assert harness.call(app, "validate(uint256)", word).abi_return is bool(word)
        else:
            assert harness.call(app, "validate(uint256)", word, expect_revert=True).reverted


@pytest.mark.parametrize("slot_layout", [False, True])
def test_struct_offset_fixed_point(harness, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/struct_offset_fixed_point.sol",
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    for cycle in (False, True):
        result = harness.call(app, "run(bool)", cycle, extra_fee=20_000).abi_return
        assert tuple(map(as_int, result)) == (0, 7)


@pytest.mark.parametrize("via_ir", [False, True])
def test_yul_scoped_facts(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/yul_scoped_facts.sol", via_yul_behavior=via_ir)
    for value in (0, 3, 6):
        result = harness.call(app, "siblings(uint64)", value).abi_return
        assert tuple(map(as_int, result)) == (value + 11, value + 22)
        result = harness.call(app, "locals(uint64)", value).abi_return
        assert tuple(map(as_int, result)) == (value + 7, value + 10)
        result = harness.call(app, "recursive(uint64)", value, extra_fee=20_000).abi_return
        total = value * (value + 1) // 2
        assert tuple(map(as_int, result)) == (total, total * 2)


@pytest.mark.parametrize("slot_layout", [False, True])
def test_storage_return_facts(harness, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/storage_return_facts.sol",
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    for read_storage in (False, True):
        result = harness.call(app, "run(bool)", read_storage, extra_fee=20_000).abi_return
        assert tuple(map(as_int, result)) == (7, 23)
    result = harness.call(app, "slots()").abi_return
    assert tuple(map(as_int, result)) == (123, 456, 789)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("contract", ["InlineConstructorPlan", "DeferredConstructorPlan"])
def test_constructor_plan(harness, via_ir, contract):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/constructor_plan.sol",
        contract_name=contract, via_yul_behavior=via_ir)
    assert as_int(harness.call(app, "total()").abi_return) == 277
    assert as_int(harness.call(app, "observed()").abi_return) == (233 if via_ir else 0)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("method", [
    "locals()", "preIncrement()", "assigned()", "nested()", "control()",
    "memoryArgs()", "blobArgs()", "memoryBinary(bool)",
])
def test_operand_effect_facts(harness, via_ir, method):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/operand_effect_facts.sol", via_yul_behavior=via_ir)
    local_expected = {
        "locals()": (11, 2), "preIncrement()": (12, 2), "assigned()": (17, 7),
        "nested()": (41, 2), "control()": (12, 1),
    }
    if method in local_expected:
        result = harness.call(app, method).abi_return
        assert tuple(map(as_int, result)) == local_expected[method]
    elif method != "memoryBinary(bool)":
        result = harness.call(app, method, extra_fee=20_000).abi_return
        assert tuple(map(as_int, result)) == (31, 9)
    else:
        for mutation_on_left in (False, True):
            result = harness.call(app, method, mutation_on_left).abi_return
            expected = 10 if mutation_on_left == via_ir else 4
            assert tuple(map(as_int, result)) == (expected, 9)


@pytest.mark.parametrize("slot_layout", [False, True])
def test_call_boundary_facts(harness, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_boundary_facts.sol",
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    assert as_int(harness.call(app, "literalCall()").abi_return) == 3
    assert as_int(harness.call(app, "literalPointer()").abi_return) == 4
    for bound, expected in ((False, 7), (True, 14)):
        result = harness.call(app, "run(bool)", bound, extra_fee=20_000).abi_return
        assert tuple(map(as_int, result)) == (0, expected)
    result = harness.call(app, "sequenced()", extra_fee=20_000).abi_return
    assert tuple(map(as_int, result)) == (1, 14)
    result = harness.call(app, "memoryReturn()").abi_return
    assert tuple(map(as_int, result)) == (4, 5, 10)


@pytest.mark.parametrize("via_ir", [False, True])
def test_function_pointer_wire_facts(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/function_pointer_wire_facts.sol", via_yul_behavior=via_ir)
    for value in (0, 17, 1 << 100, (1 << 128) - 3):
        assert as_int(harness.call(app, "staticUnsigned(uint128)", value).abi_return) == value + 1
        for first in (False, True):
            result = harness.call(app, "dynamicUnsigned(bool,uint128)", first, value).abi_return
            assert as_int(result) == value + (1 if first else 2)
    for value in (-(1 << 127), -7, 0, (1 << 127) - 3):
        assert as_signed_int(harness.call(app, "staticSigned(int128)", value).abi_return) == value + 1
        for first in (False, True):
            result = harness.call(app, "dynamicSigned(bool,int128)", first, value).abi_return
            assert as_signed_int(result) == value + (1 if first else 2)


@pytest.mark.parametrize("via_ir", [False, True])
def test_explicit_conversion_plan(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/explicit_conversion_plan.sol",
        contract_name="ExplicitConversionPlan", via_yul_behavior=via_ir)
    widths = (8, 24, 64, 72, 128, 248)
    for value in (0, 255, 1 << 64, (1 << 128) - 1, (1 << 256) - 1):
        result = harness.call(app, "unsignedWidths(uint256)", value, extra_fee=20_000).abi_return
        assert tuple(map(as_int, result)) == tuple(value % (1 << bits) for bits in widths)
    for value in (-(1 << 255), -129, -1, 0, 1 << 127):
        result = harness.call(app, "signedWidths(int256)", value, extra_fee=20_000).abi_return
        expected = tuple((value + (1 << (bits - 1))) % (1 << bits) - (1 << (bits - 1)) for bits in widths)
        assert tuple(map(as_signed_int, result)) == expected
    for value in (-128, -1, 0, 127):
        result = harness.call(app, "widenSmall(int8)", value).abi_return
        assert tuple(map(as_signed_int, result)) == (value, value)
        result = harness.call(app, "reinterpret(int128)", value).abi_return
        assert (as_int(result[0]), as_signed_int(result[1])) == (value % (1 << 128), value)
    result = harness.call(app, "bytesMagnitude(bytes8,bytes16)", b"\xff" * 8, b"\xff" * 16).abi_return
    assert tuple(map(as_int, result)) == ((1 << 64) - 1, (1 << 128) - 1)
    for value in (0, 1, 2):
        assert as_int(harness.call(app, "enumCheck(uint256)", value).abi_return) == value
    for value in (3, 1 << 64, (1 << 256) - 1):
        assert harness.call(app, "enumCheck(uint256)", value, expect_revert=True).reverted


@pytest.mark.parametrize("via_ir", [False, True])
def test_conversion_operand_evaluated_once(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/explicit_conversion_plan.sol",
        contract_name="ExplicitConversionPlan", via_yul_behavior=via_ir,
        fund_wei=8_000_000)
    roots = json.loads((harness.out_dir / "awst.json").read_text())
    contract = next(root for root in roots if root.get("name") == "ExplicitConversionPlan")
    method = next(method for method in contract["methods"]
                  if method["member_name"] == "deploymentOnce")

    def nodes(value):
        if isinstance(value, dict):
            yield value
            for child in value.values():
                yield from nodes(child)
        elif isinstance(value, list):
            for child in value:
                yield from nodes(child)

    # Address conversion used to lower the constructor once for the registry
    # and again for its fallback, duplicating queued deployment effects.
    assert sum(node.get("_type") == "CreateInnerTransaction"
               and "ApprovalProgramPages" in node.get("fields", {})
               for node in nodes(method)) == 1
    assert harness.call(app, "deploymentOnce()", extra_fee=30_000).abi_return is True
