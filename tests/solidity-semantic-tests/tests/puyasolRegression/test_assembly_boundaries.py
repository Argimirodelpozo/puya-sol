"""Declaration-lifetime Yul words and immutable, solc-shaped calldata references."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_boundary_words(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "assembly_boundaries", via_ir, profile, slot)
    app = harness.deploy(artifacts, "RawScalars")
    for method, expected in (
        ("u8", 257), ("u64", 2**64 + 1), ("u128", 2**256 - 1),
        ("boolean", 2), ("fixedByte", 2**256 - 1), ("signedByte", 257),
        ("sameBlock", 257), ("highLevelWrite", 7),
    ):
        assert invoke(harness, app, profile, method + "()") == (expected,)
    assert invoke(harness, app, profile, "highLevelRead()", returns=["uint256"] * 2) == (1, 257)
    for take in (False, True):
        assert invoke(harness, app, profile, "branch(bool)", [take]) == (257 if take else 7,)
    for count in (0, 1, 2):
        assert invoke(harness, app, profile, "loop(uint256)", [count]) == (257 if count else 7,)
    assert invoke(harness, app, profile, "parameter(uint8)", [7], ["uint256"] * 2) == (7, 263)
    assert invoke(harness, app, profile, "tupleWrites()", returns=["uint256"] * 2) == (514, 257)
    for choose in (False, True):
        assert invoke(harness, app, profile, "copies(bool)", [choose], ["uint256"] * 3) == (257, 257, 257)
    assert invoke(harness, app, profile, "increment()") == (2,)
    assert invoke(harness, app, profile, "clear()") == (0,)
    for n in (0, 257, 2**64 + 1, 2**256 - 1):
        assert invoke(harness, app, profile, "namedReturn(uint256)", [n], ["uint8"]) == (n & 255,)
    snapshot = harness.deploy(artifacts, "CalldataSnapshot")
    for method in ("fixedOffset", "acrossBlocks", "sameBlock", "control"):
        assert invoke(harness, snapshot, profile, method + "(uint256)", [7]) == (7,)
    assert invoke(harness, snapshot, profile, "dynamicOffset(uint256,uint256)", [7, 4]) == (7,)
    assert invoke(harness, snapshot, profile, "messageAlias(uint256)", [7], ["uint256"] * 2) == (7, 36)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_boundary_references(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "assembly_boundaries", via_ir, profile, slot)
    for name, signature, args, expected in (
        ("Alias", "f(bytes)", [b"abc"], (0, 3)),
        ("SliceAlias", "f(bytes)", [b"abc"], (1, 2)),
        ("Rebind", "f(bytes,bytes)", [b"a", b"bc"], (2, 98)),
        ("PointerControl", "f(bytes,bytes)", [b"a", b"bc"], (2, 98)),
        ("StaticAlias", "f(uint256[2])", [[11, 22]], (0, 11)),
        ("ArrayAlias", "f(uint256[])", [[11, 22]], (0, 2)),
        ("DynamicFixedPointer", "f(bytes[2])", [[b"a", b"bc"]], (36,)),
        ("DynamicStructPointer", "f((bytes))", [[b"abc"]], (36,)),
        ("LiveArrayLength", "f(uint256[])", [[11, 22]], (2, 2)),
        ("LiveArrayElement", "f(uint256[])", [[11, 22]], (2, 11)),
    ):
        app = harness.deploy(artifacts, name)
        assert invoke(harness, app, profile, signature, args, ["uint256"] * len(expected)) == expected
    conditional = harness.deploy(artifacts, "ConditionalSeed")
    for flag in (False, True):
        assert invoke(harness, conditional, profile, "f(bytes,bool)", [b"abc", flag]) == (3,)
    loop = harness.deploy(artifacts, "LoopSeed")
    for count in (0, 1, 2):
        assert invoke(harness, loop, profile, "f(bytes,uint256)", [b"abc", count]) == (3,)
    variants = harness.deploy(artifacts, "ReferenceVariants")
    for name in ("swap", "declare"):
        assert invoke(harness, variants, profile, name + "(bytes,bytes)",
                      [b"a", b"bc"], ["uint256"] * 2) == (2, 1)
    for take in (False, True):
        assert invoke(harness, variants, profile, "select(bytes,bytes,bool)",
                      [b"a", b"bc", take], ["uint256"] * 2) == ((1, 97) if take else (2, 98))
    assert invoke(harness, variants, profile, "typed(uint256[],uint256[])",
                  [[11, 22], [33]], ["uint256"] * 2) == (1, 33)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_boundary_call_frames(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "assembly_boundaries", via_ir, profile, slot)
    app = harness.deploy(artifacts, "InternalCalldataFrame")
    assert invoke(harness, app, profile, "run(uint256,bytes)", [7, b"abc"], ["uint256"] * 4) == (7, 101, 2, 98)
    assert invoke(harness, app, profile, "throughPublic(uint256,bytes)", [7, b"abc"], ["uint256"] * 2) == (7, 100)
    for take in (False, True):
        assert invoke(harness, app, profile, "indirect(uint256,bytes,bool)",
                      [7, b"abc", take], ["uint256"] * 4) == (7, 133, 2, 98)
        assert invoke(harness, app, profile, "mixedPointer(uint256,bytes,bool)",
                      [7, b"abc", take], ["uint256"] * 4) == ((777, 888, 2, 98) if take else (7, 133, 2, 98))
    virtual = harness.deploy(artifacts, "VirtualCalldataFrame")
    assert invoke(harness, virtual, profile, "run(uint256)", [7]) == (7,)
    fallback = harness.deploy(artifacts, "FallbackCalldataFrame")
    result = harness.call_raw(fallback, b"\x12\x34\x56\x78", extra_args=(b"\x01\x02",),
                              extra_fee=40_000, budget_pool=8)
    assert decode(["uint256"] * 4, result.logs[-1][4:]) == (6, 0, 6, 0x12)
    modified = harness.deploy(artifacts, "ModifierCalldataFrame")
    assert invoke(harness, modified, profile, "run(bytes)", [b"abc"], ["uint256"] * 2) == (97, 255)
    if profile == "evm":
        selector = keccak.new(digest_bits=256, data=b"noReturn(bytes)").digest()[:4]
        harness.call_raw(modified, selector, extra_args=(encode(["bytes"], [b"abc"]),),
                         extra_fee=40_000, budget_pool=8)
    else:
        harness.call(modified, "noReturn(bytes)", b"abc", extra_fee=40_000)
    assert invoke(harness, modified, profile, "observed()") == (97 * 256 + 254,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("binding", [
    "bytes calldata a = one(data);",
    "bytes calldata a = data; a = one(data);",
    "(bytes calldata a, bytes calldata b) = pair(data);",
    "bytes calldata a = data; bytes calldata b = data; (a, b) = pair(data);",
])
def test_returned_calldata_coordinates(harness, tmp_path, via_ir, binding):
    source = tmp_path / "ReturnedCalldata.sol"
    source.write_text("""
        pragma solidity ^0.8.20;
        contract C {
            function one(bytes calldata data) internal pure returns (bytes calldata) { return data; }
            function pair(bytes calldata data) internal pure returns (bytes calldata, bytes calldata) { return (data, data); }
            function run(bytes calldata data) external pure returns (uint256 n) {
        """ + binding + " assembly { n := a.length } } }")
    artifacts = harness.compile(source, via_yul_behavior=via_ir)
    app = harness.deploy(artifacts, "C")
    for data in (b"", b"abc"):
        assert invoke(harness, app, "arc4", "run(bytes)", [data]) == (len(data),)
