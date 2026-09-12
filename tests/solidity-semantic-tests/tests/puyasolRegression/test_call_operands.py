"""Call evaluation, allocation and storage mutations, checked against solc 0.8.34."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int


def invoke(harness, app, profile, signature, arguments=(), returns=("uint256",), *, reverts=False):
    if profile == "evm":
        inputs = signature.split("(", 1)[1][:-1]
        types = inputs.split(",") if inputs else []
        arguments = ["0x" + arg[-20:].hex() if kind == "address" else arg
                     for kind, arg in zip(types, arguments, strict=True)]
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(types, arguments),),
                                  extra_fee=40_000, budget_pool=8, expect_revert=reverts)
        if reverts:
            assert result.reverted, signature
            return
        return decode(returns, result.logs[-1][4:])
    result = harness.call(app, signature, *arguments, extra_fee=40_000, expect_revert=reverts)
    if reverts:
        assert result.reverted, signature
        return
    values = (result.abi_return,) if len(returns) == 1 else result.abi_return
    return tuple(bytes(value) if kind.startswith("bytes") else as_int(value)
                 for kind, value in zip(returns, values, strict=True))


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_call_operands(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/call_operands.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    target = harness.deploy(artifacts, "CallsTarget", fund_wei=30_000_000)
    app = harness.deploy(artifacts, "CallsProbes", fund_wei=30_000_000, postinit_budget_pool=8)
    address = target.app_id.to_bytes(32, "big")
    check = lambda sig, args=(), ret=("uint256",): invoke(harness, app, profile, sig, args, ret)
    assert check("requireMessage()") == (1,)
    assert check("requireCondition()") == (2,)
    assert check("builtin()") == (1 if via_ir else 0,)
    assert check("named()") == (10 if via_ir else 1,)
    assert check("externalArgs(address)", [address]) == (1,)
    assert check("getter(address)", [address]) == (7,)
    assert check("externalOptions(address)", [address]) == (123,)
    assert check("nestedPush()", ret=["uint256", "uint256"]) == (2, 7)
    assert check("nestedBytes()", ret=["bytes"]) == (b"\x07\x08",)
    assert check("nestedAlias()", ret=["bytes"]) == (b"\x07\x08",)
    assert check("allocateOnce()", ret=["uint256", "uint256"]) == (3, 1)
    assert check("mutableOperand()") == (71,)
    assert check("abiOperands()", ret=["uint256", "uint256"]) == (0, 1)
    assert check("packedOperands()", ret=["bytes"]) == (bytes(63) + b"\x01",)
    assert check("namedExternal(address)", [address]) == (10 if via_ir else 1,)
    assert check("namedStruct()") == (10 if via_ir else 1,)
    assert check("nestedFieldPush()", ret=["uint256"] * 3) == (2, 7, 8)
    assert check("pushedStruct()", ret=["uint256", "uint256"]) == (7, 8)
    for method in ("allocate", "allocateArray", "allocateString", "allocateBool"):
        for n in (0, 1, 8):
            assert check(method + "(uint256)", [n]) == (n,)
        for n in (2**64, 2**64 + 1, 2**256 - 1):
            invoke(harness, app, profile, method + "(uint256)", [n], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_call_conversions(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_conversions.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    for raw, n, signed in ((b"", 0, 0), (b"x", 1, -1),
                           (b"abcdef", 2**128 - 1, -(2**167)),
                           (b"\x00\xff", 2**127, 2**167 - 1)):
        expected = (b"abc\x00", (raw + bytes(4))[:4], n.to_bytes(16, "big"),
                    signed.to_bytes(21, "big", signed=True),
                    bytes.fromhex("00112233445566778899aabbccddeeff00112233"))
        assert invoke(harness, app, profile, "fixedValues(bytes,uint128,int168)",
                      [raw, n, signed], ["bytes4", "bytes4", "bytes16", "bytes21", "bytes20"]) == expected
        assert invoke(harness, app, profile, "views(bytes)", [raw], ["bytes"]) == (raw,)
    for n in (0, 1):
        assert invoke(harness, app, profile, "ordinal(uint256)", [n], ["uint8"]) == (n,)
    for n in (2, 2**64, 2**256 - 1):
        invoke(harness, app, profile, "ordinal(uint256)", [n], reverts=True)
