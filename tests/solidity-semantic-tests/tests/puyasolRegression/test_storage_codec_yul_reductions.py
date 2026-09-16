"""Shared storage codecs and bounded Yul memory: pinned-solc runtime expectations."""

import pytest
from Crypto.Hash import keccak
from eth_abi import encode

from test_ast_audit import compile_app
from test_call_operands import invoke


def digest(value):
    return keccak.new(digest_bits=256, data=value).digest()


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_storage_codec_reduction(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "storage_codec_yul_reductions", via_ir, profile, slot)
    app = harness.deploy(artifacts, "StorageCodecReductions", fund_wei=30_000_000)
    for values in ((-128, -(1 << 39), -(1 << 127)), (-1, -1, -1),
                   (0, 0, 0), (127, (1 << 39) - 1, (1 << 127) - 1)):
        assert invoke(harness, app, profile, "packed(int8,int40,int128)", values, ["bool"]) == (True,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_memory_codec_reduction(harness, via_ir, profile):
    artifacts = compile_app(harness, "storage_codec_yul_reductions", via_ir, profile, False)
    app = harness.deploy(artifacts, "MemoryCodecReductions")
    cases = (
        ("ints", "int8[9]", [-1] + [0] * 7 + [127]),
        ("bools", "bool[9]", [True] + [False] * 7 + [True]),
        ("fixedBytes", "bytes4[5]", [bytes.fromhex("11223344")] + [bytes(4)] * 3 + [bytes.fromhex("aabbccdd")]),
        ("nested", "uint64[2][5]", [[17, 0]] + [[0, 0]] * 3 + [[0, 99]]),
        ("dynamicElements", "bytes[5]", [bytes.fromhex("aa22"), b"", b"", b"", bytes.fromhex("334455")]),
    )
    for method, kind, values in cases:
        assert invoke(harness, app, profile, method + "()", returns=["bytes"]) == (encode([kind], [values]),), method
    assert invoke(harness, app, profile, "largeFixed()", returns=["bytes32"]) == (
        digest(encode(["uint256[65]"], [[11] + [0] * 63 + [99]])),)
    assert invoke(harness, app, profile, "fullFixed()", returns=["bytes32"]) == (
        digest(encode(["uint256[128]"], [[11] + [0] * 126 + [99]])),)
    for n in (0, 1, 31, 32, 33, 4065, 4095, 4096):
        assert invoke(harness, app, profile, "bytesCopy(uint256)", [n], ["uint256"] * 3) == (
            n, 97 if n else 0, 0)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_yul_reductions(harness, via_ir, profile):
    artifacts = compile_app(harness, "storage_codec_yul_reductions", via_ir, profile, False)
    app = harness.deploy(artifacts, "YulReductions")
    for x in (0, 2, 3, 8):
        assert invoke(harness, app, profile, "boolSwitch(uint256)", [x]) == (7,)
    for x in (0, 97, 98):
        assert invoke(harness, app, profile, "stringSwitch(uint256)", [x]) == (11 if x == 97 else 22,)
    assert invoke(harness, app, profile, "overlap()", returns=["bytes32"]) == (digest(bytes(32)),)
    signature = "calldataCoincidence((uint256,uint256))"
    if profile == "evm":
        result = harness.call_raw(app, digest(signature.encode())[:4],
                                  extra_args=(encode(["(uint256,uint256)"], [(11, 22)]),),
                                  extra_fee=40_000, budget_pool=8)
        actual = result.logs[-1][4:]
    else:
        actual = bytes(harness.call(app, signature, (11, 22)).abi_return)
    assert actual == digest(bytes(64))
    for n in (0, 1, 16, 32):
        assert invoke(harness, app, profile, "dynamicCoincidence(bytes)", [b"x" * n], ["bytes32"]) == (
            digest(bytes(n + 32)),)
    invoke(harness, app, profile, "hugeHash()", reverts=True)
    assert invoke(harness, app, profile, "hashRange(uint256,uint256)",
                  [2**256 - 1, 0], ["bytes32"]) == (digest(b""),)
    for offset, size in ((2**64, 1), (0, 2**64), (20479, 2)):
        invoke(harness, app, profile, "hashRange(uint256,uint256)", [offset, size], reverts=True)
    for method in ("poisonedAlignment", "previousBlockAlignment"):
        assert invoke(harness, app, profile, method + "()") == (1,)
    for x in (0, 17):
        assert invoke(harness, app, profile, "results(uint256)", [x], ["uint256"] * 3) == (
            2 * x + 4, 2 * x + 5, 2)
    for n in (0, 1, 31, 32):
        if profile == "evm":
            result = harness.call_raw(app, digest(b"revertRange(uint256)")[:4],
                                      extra_args=(encode(["uint256"], [n]),),
                                      extra_fee=40_000, budget_pool=8, expect_revert=True)
        else:
            result = harness.call(app, "revertRange(uint256)", n,
                                  extra_fee=40_000, expect_revert=True)
        assert result.reverted
        assert result.revert_data == (0x1122334455667788).to_bytes(32, "big")[:n]
