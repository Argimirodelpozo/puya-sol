"""Solc EVM layouts and ARC4 bool-run offsets have separate canonical owners."""
import pytest
from Crypto.Hash import keccak
from eth_abi import encode
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_layouts(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/assembly_layouts.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    types = ["int24[2]", "bytes4[2]", "bool[9]", "bytes[]"]
    for blobs in ([], [b""], [b"abc", b"x" * 33]):
        values = [[-7, 5], [b"abcd", b"\0\xffxy"], [True, False] * 4 + [True], blobs]
        body = encode(types, values)
        arguments = values if profile == "evm" else [[2**24 - 7, 5], *values[1:]]
        expected = (keccak.new(digest_bits=256, data=body).digest(), len(body))
        assert invoke(harness, app, profile, "calldataDigest(" + ",".join(types) + ")",
                      arguments, ["bytes32", "uint256"]) == expected
    word = 7 + sum(1 << (8 * n) for n in range(1, 10)) + ((2**24 - 7) << 80) + (2**100 << 104)
    assert invoke(harness, app, profile, "packedWord(uint256)", [word], ["uint256", "bool"]) == (word, 1)
