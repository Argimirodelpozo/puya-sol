"""Flat scope views preserve solc lexical boundaries and callable-local state."""

import pytest

from framework import as_bytes, as_int


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot_layout", [False, True], ids=["handles", "slots"])
def test_context_scopes(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/context_scopes.sol",
        contract_name="ContextScopes",
        ctor_args=[3],
        via_yul_behavior=via_ir,
        extra_args=["--evm-storage-layout"] if slot_layout else [],
    )
    # State initializers and base-constructor arguments run outside a function view.
    assert as_int(harness.call(app, "initialized()").abi_return) == 42
    assert as_int(harness.call(app, "base()").abi_return) == 22
    for method, argument, expected in (
        ("nested(uint8)", 255, 4),
        ("nested(uint8)", 253, 3),
        ("checkedAfter(uint8)", 1, 3),
        ("checkedCall(uint8)", 1, 2),
        ("repeated(uint8)", 3, 12),
        ("repeated(uint8)", 63, 252),
    ):
        if method == "repeated(uint8)":
            harness.call(app, "reset()")
        assert as_int(harness.call(app, method, argument).abi_return) == expected
    for method, argument in (
        ("checkedAfter(uint8)", 254),
        ("checkedCall(uint8)", 255),
        ("repeated(uint8)", 64),
    ):
        if method == "repeated(uint8)":
            harness.call(app, "reset()")
        assert harness.call(app, method, argument, expect_revert=True).reverted
    data = b"abcdef"
    for _ in range(2):
        assert as_bytes(harness.call(app, "shifted(bytes)", data).abi_return) == data[2:]
        assert as_bytes(harness.call(app, "original(bytes)", data).abi_return) == data


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot_layout", [False, True], ids=["handles", "slots"])
def test_modifier_super_lookup(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_super_lookup.sol",
        contract_name="ModifierSuperLookup",
        via_yul_behavior=via_ir,
        extra_args=["--evm-storage-layout"] if slot_layout else [],
    )
    # C3 order: leaf, right, left, base. A static modifier keeps its lexical owner.
    for method, expected in (
        ("inherited()", 5),
        ("explicitLeft()", 1),
        ("explicitRight()", 3),
        ("explicitBase()", 1),
    ):
        assert as_int(harness.call(app, method).abi_return) == expected
        assert as_int(harness.call(app, "trace()").abi_return) == expected
