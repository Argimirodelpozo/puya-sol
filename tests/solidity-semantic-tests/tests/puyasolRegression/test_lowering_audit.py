"""Lowering and parenthesis regressions, checked against pinned solc legacy/IR."""

import hashlib

import pytest
from test_call_operands import invoke
from test_ast_audit import compile_app


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_static_context(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_static", via_ir, profile, slot)
    app = harness.deploy(artifacts, "StaticProbe", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "internalViewNoStatic()") == (1,)
    assert invoke(harness, app, profile, "staticThenInternal()", returns=["uint256"] * 2) == (0, 1)
    assert invoke(harness, app, profile, "rawNonStatic()", returns=["bool", "bytes"]) == (
        True, (1).to_bytes(32, "big"))
    for name in ("inheritedStatic", "nestedStatic"):
        invoke(harness, app, profile, name + "()", reverts=True)
    invoke(harness, app, profile, "pointerStatic(bool)", [True], reverts=True)
    # EVM reports (false, empty); the acknowledged AVM self-call adaptation
    # aborts the outer call. In either case the hidden write must not succeed.
    for name in ("directStatic", "rawStatic"):
        invoke(harness, app, profile, name + "()", returns=["bool", "bytes"], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_calls(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_calls", via_ir, profile, slot)
    app = harness.deploy(artifacts, "Probe", fund_wei=30_000_000)
    sink = harness.deploy(artifacts, "Sink", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "internalPurePointer()") == (1,)
    address = sink.app_id.to_bytes(32, "big")
    assert invoke(harness, app, profile, "pointerOptions(address)", [address],
                  ["uint256", "uint256"]) == (7, 1100)
    assert invoke(harness, app, profile, "fakeEncoder(address)", [address]) == (22,)
    assert invoke(harness, app, profile, "foreignSelector(address)", [address]) == (11,)
    assert invoke(harness, app, profile, "foreignSelectorSum(address)", [address]) == (16,)
    assert invoke(harness, app, profile, "fakeSelfEncoder()") == (22,)
    for name in ("directSelf", "rawSelf", "rawSelectorSelf", "parenthesizedSelf"):
        assert invoke(harness, app, profile, name + "()", returns=["bool", "bytes"]) == (
            True, (42).to_bytes(32, "big"))
    assert invoke(harness, app, profile, "emptySelf()", returns=["bool", "uint256"]) == (True, 5)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_precompiles(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_precompiles", via_ir, profile, slot)
    app = harness.deploy(artifacts, "PrecompileProbe")
    for name in ("direct", "wrapped", "constantTarget"):
        assert invoke(harness, app, profile, name + "(bytes)", [b"abc"],
                      ["bool", "bytes"]) == (True, hashlib.sha256(b"abc").digest())
    assert invoke(harness, app, profile, "encodedInput()", returns=["bool", "bytes"]) == (
        True, hashlib.sha256(bytes.fromhex("26121ff0")).digest())


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_pointer_override(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_pointer_override", via_ir, profile, slot)
    app = harness.deploy(artifacts, "B", fund_wei=30_000_000)
    other = harness.deploy(artifacts, "A", fund_wei=30_000_000)
    for target, expected in ((other, 5), (app, 6)):
        assert invoke(harness, app, profile, "test(address,uint256[])",
                      [target.app_id.to_bytes(32, "big"), [5]]) == (expected,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_returndata(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_returndata", via_ir, profile, slot)
    app = harness.deploy(artifacts, "FallbackProbe")
    assert invoke(harness, app, profile, "test()", returns=["bool"] + ["uint256"] * 3) == (True, 32, 3, 3)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_parentheses(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_parentheses", via_ir, profile, slot)
    app = harness.deploy(artifacts, "Parentheses", fund_wei=30_000_000, postinit_budget_pool=8)
    assert invoke(harness, app, profile, "aliases()", returns=["uint256"] * 2) == (11, 33)
    assert invoke(harness, app, profile, "returnedReferences()") == (6,)
    assert invoke(harness, app, profile, "indexedPaths()", returns=["uint256"] * 3) == (7, 9, 0)
    assert invoke(harness, app, profile, "byteOps()", returns=["uint256"] * 2) == (3, 0)
    assert invoke(harness, app, profile, "allocation()", returns=["uint256"] * 2) == (3, 7)
    assert invoke(harness, app, profile, "singleton()") == (7,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_wire(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_wire", via_ir, profile, slot)
    app = harness.deploy(artifacts, "WireProbe", fund_wei=30_000_000)
    other = harness.deploy(artifacts, "WireSink", fund_wei=30_000_000)
    address = other.app_id.to_bytes(32, "big")
    from eth_abi import encode
    from eth_utils import keccak
    expected = keccak(encode(["uint8", "uint24", "int16", "bool", "bytes3", "string", "uint8[]"],
                             [255, 0xabcdef, -7, True, bytes.fromhex("010203"), "abc", [7, 255]]))
    for pointer in (False, True):
        assert invoke(harness, app, profile, "typed(address,bool)", [address, pointer],
                      ["bytes32"]) == (expected,)
    # .selector follows the selected profile; canonical encodeCall works in both.
    for selector in ([False, True] if profile == "evm" else [False]):
        assert invoke(harness, app, profile, "raw(address,bool)", [address, selector],
                      ["bytes32"]) == (expected,)
    assert invoke(harness, app, profile, "librarySelf()") == (42,)
