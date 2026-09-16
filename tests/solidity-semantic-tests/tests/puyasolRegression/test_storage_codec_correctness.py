"""Storage word validation and solc-shaped memory allocation/reference writes."""

import pytest
from eth_abi import encode

from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_storage_word_correctness(harness, via_ir, profile):
    artifacts = compile_app(harness, "storage_codec_correctness", via_ir, profile, True)
    lengths = harness.deploy(artifacts, "StorageLengthChecks", fund_wei=30_000_000)
    for method in ("copyFlags", "copyWords", "copyNested", "replaceFlags", "replaceWords"):
        for n in (0, 1):
            expected = 1 if method.startswith("replace") else n
            assert invoke(harness, lengths, profile, method + "(uint256)", [n]) == (expected,)
        for n in (2**64, 2**64 + 1, 2**128 + 1, 2**256 - 1):
            invoke(harness, lengths, profile, method + "(uint256)", [n], reverts=True)
    traversal = harness.deploy(artifacts, "StorageTraversalChecks", fund_wei=30_000_000)
    for method in ("roundTrip", "clearStruct", "clearPacked"):
        assert invoke(harness, traversal, profile, method + "()", returns=["bool"]) == (True,)
    addresses = harness.deploy(artifacts, "PackedAddressChecks", fund_wei=30_000_000)
    for method in ("clearRaw", "keepNeighbor", "changeRestore"):
        assert invoke(harness, addresses, profile, method + "()", returns=["bool"]) == (True,)
    for clear, typed in ((False, False), (True, False), (False, True)):
        assert invoke(harness, addresses, profile, "mapped(bool,bool)", [clear, typed], ["bool"]) == (True,)
    enums = harness.deploy(artifacts, "EnumBoundaryChecks", fund_wei=30_000_000)
    for n in (0, 1, 2, 7, 255, 257, 2**64 + 1, 2**256 - 1):
        assert invoke(harness, enums, profile, "rawEnum(uint256)", [n]) == (n,)
        for packed in (False, True):
            if n < 2:
                assert invoke(harness, enums, profile, "encodeEnum(uint256,bool)", [n, packed], ["bytes"]) == (
                    n.to_bytes(1 if packed else 32, "big"),)
            else:
                invoke(harness, enums, profile, "encodeEnum(uint256,bool)", [n, packed], reverts=True)
        for method in ("storedEnum", "explicitEnum"):
            cleaned = n & 255 if method == "storedEnum" else n
            if cleaned < 2:
                assert invoke(harness, enums, profile, method + "(uint256)", [n]) == (cleaned,)
            else:
                invoke(harness, enums, profile, method + "(uint256)", [n], reverts=True)
        for signature, args in (("namedEnum(uint256)", [n]),
                                ("callEnum(uint256,bool)", [n, False]),
                                ("callEnum(uint256,bool)", [n, True])):
            if n < 2:
                assert invoke(harness, enums, profile, signature, args, ["uint8"]) == (n,)
            else:
                invoke(harness, enums, profile, signature, args, reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_memory_layout_correctness(harness, via_ir, profile):
    artifacts = compile_app(harness, "memory_layout_correctness", via_ir, profile, False)
    app = harness.deploy(artifacts, "MemoryLayoutChecks")
    assert invoke(harness, app, profile, "nested()", returns=["bytes"]) == (
        encode(["uint64[2][5]"], [[[17, 0]] + [[0, 0]] * 3 + [[0, 99]]]),)
    assert invoke(harness, app, profile, "byteAssignments()", returns=["bytes"]) == (
        encode(["bytes[5]"], [[bytes.fromhex("aa22"), b"", b"", b"", bytes.fromhex("334455")]]),)
    for method in ("distinct", "structDefault", "freshZero", "aliases", "tupleAliases", "deletedReferences",
                   "highLevelAliases", "freshFunctionArrays"):
        assert invoke(harness, app, profile, method + "()", returns=["bool"]) == (True,)
    assert invoke(harness, app, profile, "zeroPointer()", returns=["uint256"] * 3) == (96, 96, 96)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_narrow_yul_words(harness, via_ir, profile):
    artifacts = compile_app(harness, "memory_layout_correctness", via_ir, profile, False)
    app = harness.deploy(artifacts, "NarrowYulChecks")
    for n in (0, 1, 257, 2**64 + 1, 2**256 - 1):
        for method, kind, clean in (("uintWord", "uint8", n & 255), ("boolWord", "bool", bool(n)),
                                    ("bytesWord", "bytes1", (n >> 248).to_bytes(1, "big"))):
            assert invoke(harness, app, profile, method + "(uint256)", [n], ["uint256", kind]) == (n, clean)
