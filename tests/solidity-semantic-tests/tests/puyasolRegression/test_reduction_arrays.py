"""Shared aggregate codecs and counted fixed-storage traversals."""

import pytest
from eth_abi import encode
from Crypto.Hash import keccak

from test_call_operands import invoke


def digest(data):
    return keccak.new(digest_bits=256, data=data).digest()


def encoded_hash(types, values):
    return digest(encode(types, values))


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_reduction_array_loops(harness, via_ir, profile):
    artifacts = harness.compile("puyasolRegression/contracts/reduction_arrays.sol",
        via_yul_behavior=via_ir, extra_args=["--contract-abi", profile, "--evm-storage-layout"])
    app = harness.deploy(artifacts, "ReductionArrays", fund_wei=40_000_000)
    values = [0, 2**64 - 1, *range(2, 16)]
    for _ in range(2):
        assert invoke(harness, app, profile, "fixedCopy(uint64[16])", [values], ["bytes32"]) == (
            encoded_hash(["uint64[16]", "uint64[16]"], [values, [0] * 16]),)
        assert invoke(harness, app, profile, "shortCopy()", returns=["bytes32"]) == (
            encoded_hash(["uint64[16]"], [[11, 0, 0, 44] + [0] * 12]),)
        flags = [(i % 3) == 0 for i in range(16)]
        assert invoke(harness, app, profile, "boolCopy(bool[16])", [flags], ["bytes32"]) == (
            encoded_hash(["bool[16]", "bool[16]"], [flags, [False] * 16]),)
        assert invoke(harness, app, profile, "dynamicChildren()", returns=["bytes32", "bytes32"]) == (
            encoded_hash(["string[8]"], [["first", "", "", "last", "", "", "", ""]]),
            encoded_hash(["string[8]"], [[""] * 8]))
        assert invoke(harness, app, profile, "tupleCopy()", returns=["uint256", "uint256", "uint128", "uint128"]) == (11, 17, 13, 13)
        assert invoke(harness, app, profile, "clearStruct()", returns=["uint256", "uint128"]) == (0, 0)

    values = list(range(8))
    texts = ["", "a", "b" * 31, "c" * 32, "d" * 33, "λ", "last", "old"]
    wire = encode(["uint16[8]", "string[8]"], [values, texts])
    assert invoke(harness, app, profile, "decoded(bytes)", [wire], ["bytes32", "bytes32"]) == (
        digest(wire), encoded_hash(["uint16[8]", "string[8]"], [values, [*texts[:-1], "changed"]]))
    for malformed in (b"", wire[:31], wire[:-33]):
        invoke(harness, app, profile, "decoded(bytes)", [malformed], reverts=True)
    assert invoke(harness, app, profile, "dirty(uint16[16])", [list(range(16))], ["bytes32"]) == (
        encoded_hash(["uint16[16]"], [[*range(15), 9]]),)
