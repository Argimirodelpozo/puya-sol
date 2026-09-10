"""Conversion plans, ARC4 defaults, array strategies and evaluation identity."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot_layout", [False, True], ids=["named", "slot"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_soltypes_conversions(harness, via_ir, slot_layout, profile):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/soltypes_conversions.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))

    def check(signature, arguments, expected, returns):
        if profile == "evm":
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),), extra_fee=40_000)
            actual = decode(returns, result.logs[-1][4:])
        else:
            result = harness.call(app, signature, *arguments, extra_fee=40_000)
            values = (result.abi_return,) if len(returns) == 1 else result.abi_return
            actual = tuple(bytes(v) if t.startswith("bytes") else v if t == "string" else
                           as_signed_int(v) if t.startswith("int") else as_int(v)
                           for t, v in zip(returns, values, strict=True))
        assert actual == expected, (signature, arguments, actual, expected)

    for value in (-128, -7, 0, 127):
        check("construct(int8,bytes3)", [value, b"abc"], (value, b"abc\0\0", True, "hello"),
              ["int128", "bytes5", "bool", "string"])
        check("pushValue(int8)", [value], (value, value, 2), ["int256", "int256", "uint256"])
        check("pushStruct(int8)", [value], (value, b"\x01\x02\x03\0\0", "push"),
              ["int256", "bytes5", "string"])
        check("receiver(int8)", [value], (value,), ["int256"])
        for choose in (False, True):
            check("forwarded(int8,bool)", [value, choose], (value if choose else 7, -2, 1),
                  ["int128", "int256", "uint64"])
    check("fixedCopy()", [], (1, 7, 255, 0), ["uint256"] * 4)
    check("literalTuple()", [], (b"hi\0\0\0", b"\0\xff\0\0", b"\0\0\0", b"a\0\xff\0", -128, 1),
          ["bytes5", "bytes4", "bytes3", "bytes4", "int128", "uint64"])
    for count in (1, 3):
        check("projection(uint16,uint8)", [65535, count], (9, count, 65535),
              ["uint16", "uint256", "uint16"])
    check("defaults()", [], (False, False, 0, 0, 0, False),
          ["bool", "bool", "uint256", "uint256", "uint256", "bool"])


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_soltypes_array_loop(harness, via_ir, profile):
    # The slot storage codec caps whole-array writes at 64 elements; exercise
    # the >256-element converter strategy on the named-storage backend.
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/soltypes_array_loop.sol",
        via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    if profile == "evm":
        selector = keccak.new(digest_bits=256, data=b"loopCopy()").digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(b"",), extra_fee=100_000)
        actual = decode(["uint256"] * 3, result.logs[-1][4:])
    else:
        result = harness.call(app, "loopCopy()", extra_fee=100_000)
        actual = tuple(as_int(value) for value in result.abi_return)
    assert actual == (17, 255, 0)
