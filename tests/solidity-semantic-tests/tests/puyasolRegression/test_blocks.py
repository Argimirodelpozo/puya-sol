"""Block scopes, fallthrough and solc-typed try-success bindings."""

import pytest
from algosdk.encoding import encode_address
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_block_scopes_and_fallthrough(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/block_flow.sol", contract_name="BlockFlow",
        via_yul_behavior=via_ir, extra_args=["--evm-storage-layout"] if slot_layout else [])
    for signature, args, expected in (
        ("scopes(uint8)", [4], 13), ("uncheckedNested(uint8)", [255], 0),
        ("checkedAfter(uint8)", [4], 5), ("nestedReturn(uint64)", [8], 9),
        ("forTransfers()", [], 2), ("nestedLoops()", [], 26),
    ):
        assert as_int(harness.call(app, signature, *args).abi_return) == expected
    for flag in (False, True):
        for signature, expected in (
            ("branchHalt(bool)", 11 if flag else 22),
            ("bothHalt(bool)", 31 if flag else 32),
            ("forHalt(bool)", 41 if flag else 42),
            ("doHalt(bool)", 51 if flag else 52),
            ("doTransfers(bool)", 0 if flag else 3),
            ("braceless(bool)", 7),
        ):
            assert as_int(harness.call(app, signature, flag).abi_return) == expected
    assert as_int(harness.call(app, "nestedRevert(bool)", False).abi_return) == 61
    assert harness.call(app, "nestedRevert(bool)", True, expect_revert=True).reverted
    assert harness.call(app, "checkedAfter(uint8)", 255, expect_revert=True).reverted


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_try_success_bindings_and_abort(harness, via_ir, slot_layout, profile):
    artifacts = harness.compile(
        "puyasolRegression/contracts/block_try.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))
    assert "[allowed AVM adaptation: try/catch]" in (harness.out_dir / "puya-sol.log").read_text()
    app = harness.deploy(artifacts, "BlockTryCaller")
    other = harness.deploy(artifacts, "BlockTryCallee")
    target = (encode_address(other.app_id.to_bytes(32, "big")) if profile == "arc4"
              else "0x" + other.app_id.to_bytes(20, "big").hex())

    def call(signature, returns, arguments=(), *, fail=False):
        if profile == "arc4":
            result = harness.call(app, signature, *arguments, extra_fee=30_000, expect_revert=fail)
        else:
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),),
                                      extra_fee=30_000, expect_revert=fail)
        if fail:
            assert result.reverted
            return None
        assert not result.reverted, result.fail_message
        if profile == "arc4":
            return result.abi_return
        values = decode(returns, result.logs[-1][4:])
        return values[0] if len(values) == 1 else values

    assert as_signed_int(call("one(address)", ["int16"], [target])) == -123
    result = call("tupleValue(address)", ["int16", "uint8", "bytes"], [target])
    assert (as_signed_int(result[0]), as_int(result[1]), bytes(result[2])) == (-123, 251, b"\x00\xff\x01\x02")
    result = call("unnamed(address)", ["uint8", "bytes"], [target])
    assert (as_int(result[0]), bytes(result[1])) == (251, b"\x00\xff\x01\x02")
    assert as_int(call("omitted(address)", ["uint64"], [target])) == 7
    assert as_int(call("voidReturn(address)", ["uint64"], [target])) == 11
    assert as_signed_int(call("nested(address)", ["int16"], [target])) == -241
    assert as_signed_int(call("selfTry()", ["int16"])) == -22
    assert as_int(call("acceptedFailure(address,bool)", ["uint64"], [target, False])) == 1
    # Accepted AVM divergence: catches cannot recover failed inner transactions.
    call("acceptedFailure(address,bool)", ["uint64"], [target, True], fail=True)
    assert as_int(call("count()", ["uint64"])) == 1
