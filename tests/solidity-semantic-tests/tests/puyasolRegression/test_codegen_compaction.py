"""Code-size reductions must preserve boundary checks and exact revert bytes."""

import json

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode


def uint_result(result, profile):
    assert not result.reverted, result.fail_message
    return decode(["uint256"], result.logs[-1][4:])[0] if profile == "evm" else int(result.abi_return)


def call_result(harness, app, profile, signature, args=(), *, reverts=False, types=None, **kwargs):
    if profile == "evm":
        if types is None:
            parameters = signature.split("(", 1)[1][:-1]
            types = parameters.split(",") if parameters else []
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        return harness.call_raw(
            app, selector, extra_args=[encode(types, args)],
            expect_revert=reverts, extra_fee=40_000, budget_pool=8, **kwargs)
    return harness.call(app, signature, *args, expect_revert=reverts, extra_fee=40_000, **kwargs)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_compact_revert_payloads(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/revert_payload_compaction.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))

    def check(signature, args, message):
        result = call_result(harness, app, profile, signature, args, reverts=True)
        assert result.reverted
        assert result.revert_data == bytes.fromhex("08c379a0") + encode(["string"], [message])

    for index, text in enumerate(("", "short", "1234567890123456789012345678901234",
                                   "12345678901234567890123456789012345", "café λ", "a\0b")):
        check("literal(uint256)", [index], text)
    for text in ("", "x", "a" * 31, "b" * 32, "c" * 33, "d" * 64, "e" * 95, "λ\0🙂"):
        check("dynamicRequire(bool,string)", [False, text], text)
        check("dynamicRevert(string)", [text], text)
        check("libraryRevert(string)", [text], text)
        assert not call_result(harness, app, profile, "dynamicRequire(bool,string)", [True, text]).reverted
    for expected in (1, 2):
        result = call_result(harness, app, profile, "passing()")
        assert uint_result(result, profile) == expected

    roots = json.loads((harness.out_dir / "awst.json").read_text())
    helpers = [root for root in roots if root.get("id") == "__puyasol_error_string"]
    assert len(helpers) == 1
    assert helpers[0]["inline"] is False


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_entry_guard_compaction(harness, via_ir, profile, slot):
    artifacts = harness.compile(
        "puyasolRegression/contracts/entry_guard_compaction.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "EntryGuardCompaction")

    # Native ARC4 and its EVM compatibility aliases must both enforce the
    # external boundary, without guarding internal public calls a second time.
    total = 0
    for transport in (["arc4", "evm"] if profile == "arc4" else ["evm"]):
        for signature, args in (("price(uint256)", [3]), ("plain(uint256)", [10]),
                                ("total()", []), ("skipped()", [])):
            result = call_result(harness, app, transport, signature, args,
                                 payment_wei=7, reverts=True)
            assert result.reverted
        assert uint_result(call_result(harness, app, transport, "total()"), transport) == total
        assert uint_result(call_result(harness, app, transport, "price(uint256)", [3]), transport) == 7
        total += 21  # price(3) twice, plus the incoming payment of seven.
        result = call_result(harness, app, transport, "paid(uint256)", [3], payment_wei=7)
        assert uint_result(result, transport) == total

    resources = {"extra_fee": 40_000, "budget_pool": 8}
    assert not harness.call_raw(app, None, payment_wei=11, **resources).reverted  # receive
    total += 11
    assert harness.call_raw(app, b"\xde\xad\xbe\xef", payment_wei=9, expect_revert=True).reverted
    assert not harness.call_raw(app, b"\xde\xad\xbe\xef", **resources).reverted  # nonpayable fallback
    total += 100
    assert uint_result(call_result(harness, app, profile, "total()"), profile) == total

    fallback = harness.deploy(artifacts, "PayableFallbackCompaction")
    for selector in (None, b"\xde\xad\xbe\xef"):
        assert not harness.call_raw(fallback, selector, payment_wei=13, **resources).reverted
    assert uint_result(call_result(harness, fallback, profile, "total()"), profile) == 26


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_aggregate_compaction(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/aggregate_compaction.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    record_type = "(uint8,int16,bool,bytes3,string,uint16[2],uint32[])"

    def digest(data):
        return keccak.new(digest_bits=256, data=data).digest()

    def invoke(signature, arguments=(), types=None, returns=("bytes",)):
        result = call_result(harness, app, profile, signature, arguments, types=types)
        assert not result.reverted, result.fail_message
        if not returns:
            return None
        if profile == "evm":
            values = decode(returns, result.logs[-1][4:])
            return values[0] if len(values) == 1 else values
        return result.abi_return

    # Both storage encodings, packed scalar lanes, fixed/dynamic child arrays,
    # and the 31/32-byte storage-string boundary (including shrinking a value).
    for length in (0, 31, 32, 65, 1):
        record = (251, -123, False, b"tag", "x" * length, (17, 65535), (1, 2**32 - 1))
        wire = encode([record_type], [record])
        invoke(f"store(uint256,{record_type})", [0, record], ["uint256", record_type], ())
        for signature in ("encoded(uint256)", "libraryEncoded(uint256)"):
            assert bytes(invoke(signature, [0])) == wire
        invoke("copy(uint256,uint256)", [0, 1], returns=())
        assert bytes(invoke("encoded(uint256)", [1])) == wire
        first, second = invoke("changed(uint256)", [0], returns=("bytes32", "bytes32"))
        assert bytes(first) == digest(wire)
        assert bytes(second) == digest(encode([record_type], [(6, *record[1:])]))
        assert bytes(invoke("encoded(uint256)", [1])) == wire  # no aliasing the copy
        assert bytes(invoke("sequenced(uint256)", [0])) == encode(
            [record_type, "uint256"], [(42, *record[1:]), 9])
        if profile == "evm":
            assert invoke("get(uint256)", [1], returns=[record_type]) == record

        first, second = invoke(f"memorySnapshots({record_type})", [record], [record_type], ("bytes32", "bytes32"))
        assert bytes(first) == digest(wire)
        clean = (5, -2, True, b"abc", record[4], (9, 65535), record[6])
        assert bytes(second) == digest(encode([record_type], [clean]))

    if profile == "evm":
        # Memory cleanup must not weaken the calldata decoder's trust boundary.
        selector = keccak.new(digest_bits=256, data=f"memorySnapshots({record_type})".encode()).digest()[:4]
        dirty = bytearray(encode([record_type], [record]))
        dirty[32:64] = (0x105).to_bytes(32, "big")
        assert harness.call_raw(app, selector, extra_args=[bytes(dirty)], expect_revert=True).reverted

    roots = json.loads((harness.out_dir / "awst.json").read_text())
    for prefix in (["abi_encode_", "memory_decode_"] +
                   (["storage_encode_", "storage_decode_"] if slot else [])):
        helpers = [root for root in roots if root.get("id", "").startswith("__puyasol_" + prefix)]
        assert helpers, prefix
        assert all(root["inline"] is False for root in helpers)
        if prefix != "abi_encode_":
            assert all(not root["pure"] for root in helpers)
