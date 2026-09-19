"""Construction, consumption and termination; checked against both solc 0.8.34 backends."""

import pytest
from algosdk import abi
from Crypto.Hash import keccak

from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_constructed_memory_identity(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "memory_ownership", via_ir, profile, slot)
    app = harness.deploy(artifacts, "OwnershipAudit")
    for name, expected in (
        ("freshStruct", (9, 9)), ("returnedStruct", (9, 9)),
        ("nestedStruct", (9, 9, 9)), ("assignedMember", (9, 9)),
        ("inlineArray", (9, 9)), ("temporaryMember", (9,)),
        ("arrayElement", (9,)), ("identityControl", (9,)),
    ):
        assert invoke(harness, app, profile, name + "()", returns=["uint256"] * len(expected)) == expected
    for flag in (False, True):
        assert invoke(harness, app, profile, "freshConditional(bool)", [flag], ["uint256"] * 2) == (
            (9, 9) if flag else (5, 9))


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_recursive_constructed_memory_identity(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "recursive_memory_ownership", via_ir, profile, slot)
    app = harness.deploy(artifacts, "RecursiveOwnershipAudit")
    for count in (1, 3):
        assert invoke(harness, app, profile, "aliases(uint8)", [count],
                      ["uint16", "uint16", "uint256", "uint16", "uint16"]) == (7, 11, 0, 13, 9)
    invoke(harness, app, profile, "aliases(uint8)", [0], reverts=True)
    assert invoke(harness, app, profile, "assignmentBounds(uint8)", [0]) == (7,)
    invoke(harness, app, profile, "assignmentBounds(uint8)", [1], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_memory_receiver_evaluated_once(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "memory_evaluation", via_ir, profile, slot)
    app = harness.deploy(artifacts, "EvaluationAudit")
    for name, expected in (
        ("indexedOnce", (1, 7)), ("assignedOnce", (1,)), ("discardedOnce", (1,)),
        ("lengthControl", (1, 1)), ("localControl", (1, 7)),
    ):
        assert invoke(harness, app, profile, name + "()", returns=["uint256"] * len(expected)) == expected


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_discarded_expression_effects(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "discarded_memory_effects", via_ir, profile, slot)
    app = harness.deploy(artifacts, "EffectsAudit")
    for signature, valid, invalid in (
        ("discardIndex(uint256)", 0, 1), ("discardBytes(uint256)", 0, 1),
        ("discardEnum(uint256)", 0, 2), ("discardAdd(uint256)", 0, (1 << 256) - 1),
        ("discardNegate(int256)", 0, -(1 << 255)),
        ("discardDecode(bytes)", (32).to_bytes(32, "big") + bytes(32), b""),
        ("discardTuple(uint256)", 1, 0),
    ):
        assert invoke(harness, app, profile, signature, [valid]) == (7,)
        invoke(harness, app, profile, signature, [invalid], reverts=True)
    invoke(harness, app, profile, "discardTuple(uint256)", [(1 << 256) - 1], reverts=True)
    for name in ("shortCircuit", "shortCircuitOr", "discardedConditional"):
        for flag in (False, True):
            reverts = not flag if name == "shortCircuitOr" else flag
            result = invoke(harness, app, profile, name + "(bool)", [flag], reverts=reverts)
            if not reverts:
                assert result == (7,)
    assert invoke(harness, app, profile, "leaveControl()") == (10,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_raw_yul_halt(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "discarded_memory_effects", via_ir, profile, slot)
    app = harness.deploy(artifacts, "EffectsAudit")
    for name in ("haltControl", "returnControl"):
        selector = (keccak.new(digest_bits=256, data=(name + "()").encode()).digest()[:4]
            if profile == "evm" else abi.Method.from_signature(name + "()uint256").get_selector())
        result = harness.call_raw(app, selector, extra_args=(b"",) if profile == "evm" else (),
                                  extra_fee=40_000, budget_pool=8)
        assert not result.reverted
        returns = [log[4:] for log in result.logs if log.startswith(bytes.fromhex("151f7c75"))]
        assert not returns or returns[-1] == b""


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_raw_return_call_frames(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "raw_return_frames", via_ir, profile, slot)
    app = harness.deploy(artifacts, "RawReturnFrames")
    for name, expected in (("typedWord", 8), ("typedNarrow", 8), ("pointerWord", 8), ("typedStop", 17)):
        assert invoke(harness, app, profile, name + "()") == (expected,)
    assert invoke(harness, app, profile, "typedPair()", returns=["uint256", "bytes3"]) == (7, b"\x01\x02\x03")
    invoke(harness, app, profile, "typedEmpty()", reverts=True)
    for which, data, marker in ((0, b"", 10), (1, b"*", 10), (2, b"", 11), (3, b"+", 11),
                                (4, b"", 11), (5, b"", 11)):
        assert invoke(harness, app, profile, "lowLevel(uint256)", [which], ["bool", "bytes", "uint256"]) == (
            True, data, marker)
    selector = (keccak.new(digest_bits=256, data=b"runtimeEmpty(uint256)").digest()[:4]
                if profile == "evm" else abi.Method.from_signature("runtimeEmpty(uint256)uint256").get_selector())
    result = harness.call_raw(app, selector, extra_args=(bytes(32),), extra_fee=40_000, budget_pool=8)
    assert not result.reverted
    assert result.logs[-1] == bytes.fromhex("151f7c75")


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_fresh_value_only_returns(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "memory_materialization", via_ir, profile, slot)
    for name, signature, arguments, expected in (
        ("LiteralDirect", "f()", [], 4), ("LiteralReturn", "f()", [], 4),
        ("StructDirect", "f(uint256)", [5], 7), ("StructReturn", "f(uint256)", [5], 7),
        ("IgnoredReturn", "f()", [], 7), ("NoIgnoredReturn", "f()", [], 7),
    ):
        app = harness.deploy(artifacts, name)
        assert invoke(harness, app, profile, signature, arguments) == (expected,)
        if name == "StructReturn":
            assert invoke(harness, app, profile, "aliasResult()") == (9,)
            assert invoke(harness, app, profile, "rebindResult()", returns=["uint256"] * 2) == (2, 9)
