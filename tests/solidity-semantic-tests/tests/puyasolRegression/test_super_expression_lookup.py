"""Exact solc lookup must survive modifiers, constructors and captured references."""

import pytest
from Crypto.Hash import keccak
from eth_abi import encode

from framework import as_int


def _call(harness, app, abi, signature, args=()):
    if abi == "arc4":
        return as_int(harness.call(app, signature, *args).abi_return)
    # These fixtures only expose scalar arguments and one scalar return.
    types = signature.partition("(")[2][:-1].split(",") if args else []
    selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
    result = harness.call_raw(app, selector, extra_args=(encode(types, args),), extra_fee=20_000)
    assert not result.reverted, result.fail_message
    assert result.logs[-1][:4] == bytes.fromhex("151f7c75")
    return int.from_bytes(result.logs[-1][4:], "big")


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot_layout", [False, True], ids=["handles", "slots"])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_super_expression_lookup(harness, via_ir, slot_layout, abi):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/super_expression_lookup.sol",
        contract_name="SuperExpressionLookup",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )
    # Independently executed with solc 0.8.34 in both code-generation pipelines.
    for method, args, expected in (
        ("combined()", (), 1357),
        ("modifierArgument()", (), 5),
        ("inheritedVirtual()", (), 7),
        ("trace()", (), 1),
        ("mixedCalls()", (), 35571),
        ("localPointers()", (), 517),
        ("passedPointers()", (), 517),
        ("selfPointer()", (), 7),
        ("publicBasePointer()", (), 1),
        ("returnedPointers()", (), 13),
        ("dynamicPointer(bool)", (True,), 5),
        ("dynamicPointer(bool)", (False,), 1),
        ("sameImplementation()", (), 1),
        ("memoryModifiers(uint256)", (1,), 1111),
        ("memoryModifiers(uint256)", (20,), 1130),
    ):
        assert _call(harness, app, abi, method, args) == expected, method


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot_layout", [False, True], ids=["handles", "slots"])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_super_constructor_expressions(harness, via_ir, slot_layout, abi):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/super_expression_lookup.sol",
        contract_name="SuperConstructorExpressions",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )
    for method, expected in (
        ("argument()", 5), ("leftInit()", 1), ("leftBody()", 1),
        ("rightInit()", 3), ("rightBody()", 3),
        ("leafInit()", 5), ("modifierValue()", 5),
    ):
        assert _call(harness, app, abi, method) == expected, method
