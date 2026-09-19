"""Cross-cutting ownership, wire boundaries and initialization regressions."""

import pytest
from test_ast_audit import compile_app
from test_call_operands import invoke
from test_root_inventory import compile_source
from framework import as_signed_int


@pytest.mark.parametrize("modified", [False, True])
def test_helper_ownership_is_independent_of_contract_order(tmp_path, modified):
    quiet = "contract Quiet { modifier wrap() { _; } function f(uint256 x) external pure " + (
        "wrap " if modified else "") + "returns(uint256) { return x + 1; } }"
    noise = "contract Noise { function f() external pure { assembly { return(0, 0) } } }"
    binaries = []
    reservations = []
    for source in (quiet, noise + quiet, quiet + noise):
        result, roots, output = compile_source(tmp_path, source, backend=True)
        assert result.returncode == 0, result.stdout + result.stderr
        binaries.append((output / "Quiet.approval.bin").read_bytes())
        reservations.append(next(root for root in roots if root.get("name") == "Quiet")["reserved_scratch_space"])
    assert binaries[0] == binaries[1] == binaries[2]
    assert reservations[0] == reservations[1] == reservations[2]


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_child_initialization(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "child_initialization", via_ir, profile, slot)
    child = harness.deploy(artifacts, "DeferredChild", fund_wei=20_000_000)
    assert invoke(harness, child, profile, "value()") == (9,)
    assert invoke(harness, child, profile, "immutableValue()") == (11,)
    parent = harness.deploy(artifacts, "ChildInitialization", fund_wei=30_000_000)
    assert invoke(harness, parent, profile, "createChild()", returns=["uint256"] * 2) == (9, 11)
    assert invoke(harness, parent, profile, "createMappingChild()") == (13,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_calldata_reference_returns(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "calldata_returns", via_ir, profile, slot)
    app = harness.deploy(artifacts, "CalldataReturns")
    for data in (b"", b"abc"):
        for name in ("length", "namedLength"):
            assert invoke(harness, app, profile, name + "(bytes)", [data]) == (len(data),)
        for name in ("copy", "publicReference"):
            assert invoke(harness, app, profile, name + "(bytes)", [data], ["bytes"]) == (data,)
        assert invoke(harness, app, profile, "memoryWriteBack(bytes)", [data], ["uint256"] * 2) == (1, len(data))
    assert invoke(harness, app, profile, "sliced(bytes)", [b"abc"], ["uint256", "uint256", "bytes1"]) == (1, 2, b"b")
    for flag in (False, True):
        assert invoke(harness, app, profile, "indirect(bytes,bool)", [b"abc", flag], ["uint256"] * 2) == (
            (0, 3) if flag else (1, 2))
        assert invoke(harness, app, profile, "rebound(bytes,bytes,bool)", [b"abc", b"d", flag]) == ((3,) if flag else (1,))
        assert invoke(harness, app, profile, "tupleReturn(bytes,bytes,bool)", [b"abc", b"d", flag], ["uint256"] * 2) == (
            (3, 1) if flag else (1, 3))
    assert invoke(harness, app, profile, "tupleCopy(bytes,bytes)", [b"abc", b"d"], ["bytes"] * 2) == (b"abc", b"d")
    for offset, length in ((0, 0), (2**64, 7), (2**256 - 1, 2**256 - 1)):
        assert invoke(harness, app, profile, "forged(bytes,uint256,uint256)", [b"abc", offset, length], ["uint256"] * 2) == (
            offset, length)
    assert invoke(harness, app, profile, "forgedRead(bytes,uint256,uint256)", [b"abc", 2**64, 1], ["bytes1"]) == (b"\x00",)
    invoke(harness, app, profile, "forgedRead(bytes,uint256,uint256)", [b"abc", 0, 0], reverts=True)
    assert invoke(harness, app, profile, "discardForged(bytes)", [b""]) == (7,)
    assert invoke(harness, app, profile, "discardBounds(bytes)", [b"a"]) == (7,)
    invoke(harness, app, profile, "discardBounds(bytes)", [b""], reverts=True)
    assert invoke(harness, app, profile, "stringLength(string)", ["abc"]) == (3,)
    assert invoke(harness, app, profile, "viewLength(bytes)", [b"abc"], ["uint256"] * 2) == (3, 0)
    assert invoke(harness, app, profile, "arrayLength(uint256[])", [[7, 9]], ["uint256"] * 2) == (2, 7)
    assert invoke(harness, app, profile, "fixedArray(uint256[2])", [[7, 9]], ["uint256"] * 2) == (7, 9)
    assert invoke(harness, app, profile, "records((uint256)[])", [[[11]]]) == (11,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_library_calldata_copy_boundary(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "calldata_library_returns", via_ir, profile, slot)
    app = harness.deploy(artifacts, "LibraryCalldataReturns")
    assert invoke(harness, app, profile, "fromMemory()", returns=["bytes1", "bytes"]) == (b"a", b"\xffb")
    for name in ("fromCalldata", "usingFor"):
        assert invoke(harness, app, profile, name + "(bytes)", [b"abc"], ["bytes"]) == (b"abc",)
    assert invoke(harness, app, profile, "tupleCopy(bytes,bytes)", [b"ab", b"c"], ["bytes"] * 2) == (b"ab", b"c")
    assert invoke(harness, app, profile, "immutableInput(bytes)", [b"ab"], ["bytes1", "bytes"]) == (b"a", b"ab")
    assert invoke(harness, app, profile, "coordinates(bytes)", [b"abc"], ["uint256"] * 3) == (68, 3, 68)
    assert invoke(harness, app, profile, "selector(bytes)", [b"abc"], ["bool"]) == (True,)
    if profile == "evm":
        assert invoke(harness, app, profile, "libraryMessage(bytes)", [b"abc"], ["bool"]) == (True,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_native_return_boundaries(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "shared_return_plan", via_ir, profile, slot)
    app = harness.deploy(artifacts, "SharedReturnPlan")
    assert invoke(harness, app, profile, "tinyGetterCall()", returns=["uint8"]) == (7,)
    result = invoke(harness, app, profile, "getterCalls()", returns=["int8", "int72", "uint128"])
    assert (as_signed_int(result[0], 256), as_signed_int(result[1], 256), result[2]) == (-7, -9, 2**100)
    for name in ("pair", "internalPair", "externalPair"):
        result = invoke(harness, app, profile, name + "(int8)", [-7], ["int8", "uint128"])
        assert (as_signed_int(result[0], 256), result[1]) == (-7, 2**100)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_calldata_return_preserves_byte_view(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "calldata_views", via_ir, profile, slot)
    app = harness.deploy(artifacts, "CalldataViews")
    for name in ("message", "namedMessage", "pointerMessage", "rebind"):
        for flag in (False, True):
            assert invoke(harness, app, profile, name + "(uint256,bool)", [1234, flag], ["bool", "uint256"]) == (
                True, 37 if profile == "arc4" else 68)
    for message in (False, True):
        assert invoke(harness, app, profile, "mixed(bytes,bool)", [b"abc", message], ["bool", "uint256"]) == (
            True, (10 if profile == "arc4" else 132) if message else 3)
    assert invoke(harness, app, profile, "tupleViews(bytes,bool)", [b"abc", True], ["bool"] * 2) == (True, True)
    assert invoke(harness, app, profile, "defaults(uint256,bool)", [1234, True], ["uint256"] * 4) == (68, 68, 68, 0)
    assert invoke(harness, app, profile, "resizeMessage(uint256,bool)", [1234, True], ["bool", "uint256"]) == (True, 37)
