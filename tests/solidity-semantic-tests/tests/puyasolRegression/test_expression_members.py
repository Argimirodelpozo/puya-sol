"""Member/access-path regressions, checked against solc 0.8.34 in both codegens."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.fixture(params=[False, True], ids=["legacy", "via-ir"])
def via_ir(request):
    return request.param


@pytest.fixture(params=[False, True], ids=["named", "slot"])
def slot_layout(request):
    return request.param


@pytest.fixture(params=["arc4", "evm"])
def profile(request):
    return request.param


def deploy(harness, fixture, via_ir, slot_layout, profile):
    app = harness.compile_and_deploy(
        f"puyasolRegression/contracts/{fixture}.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []),
        fund_wei=80_000_000, postinit_budget_pool=14)

    def check(signature, arguments, expected, returns, inputs=None):
        if profile == "arc4":
            result = harness.call(app, signature, *arguments, expect_revert=expected is None, extra_fee=80_000)
        else:
            inputs = inputs if inputs is not None else (
                signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else [])
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),),
                                      expect_revert=expected is None, extra_fee=80_000, budget_pool=14)
        assert result.reverted == (expected is None), (signature, result.fail_message)
        if expected is None:
            return
        if profile == "evm":
            actual = decode(returns, result.logs[-1][4:])
        else:
            values = (result.abi_return,) if len(returns) == 1 else result.abi_return
            actual = tuple(bytes(value) if kind.startswith("bytes") else
                           as_signed_int(value) if kind.startswith("int") else as_int(value)
                           for kind, value in zip(returns, values, strict=True))
        assert actual == expected, (signature, arguments, actual, expected)

    return check


def test_expression_members(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "expression_members", via_ir, slot_layout, profile)
    check("constants()", [], (b"\x12\0\0\0", 4, b"\0", -7, True),
          ["bytes4", "uint256", "bytes1", "int256", "bool"])
    check("dynamicSelector()", [], (True, 1), ["bool", "uint256"])
    check("optionsSelector()", [], (True, 11), ["bool", "uint256"])
    check("fixedLength()", [], (4, 1), ["uint256", "uint256"])
    for yes in (False, True):
        check("mixedSelector(bool)", [yes], (True, 11 if yes else 1), ["bool", "uint256"])
        for different in (False, True):
            check("conditionalSelector(bool,bool)", [yes, different],
                  (True, 11 if yes else 101), ["bool", "uint256"])
    for start, end in ((0, 0), (0, 3), (1, 3), (3, 2), (0, 4), (0, 2**64 + 1), (2**200, 2**200 + 1)):
        check("sliceLength(uint256[],uint256,uint256)", [[7, 11, 13], start, end],
              (end - start,) if start <= end <= 3 else None, ["uint256"])
        check("nestedLength(uint256[],uint256,uint256)", [[7, 11, 13], start, end],
              (end - start,) if start <= end <= 2 else None, ["uint256"])


def test_expression_member_storage(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "expression_member_storage", via_ir, slot_layout, profile)
    check("lengths()", [], (2, 3, 1), ["uint256"] * 3)


def test_expression_assignment_paths(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "expression_assignment_paths", via_ir, slot_layout, profile)
    for compound in (False, True):
        check("paged(bool)", [compound], (11 if compound else 1, 2), ["uint256"] * 2)
    check("nestedPaged()", [], (12, 9, 1), ["uint256"] * 3)
    check("boxed()", [], (1, 2), ["uint256"] * 2)
    check("tupleStores()", [], (11, 22, 2, 7, 10), ["uint256"] * 5)
    check("pagedIncrement()", [], (7, 8, 1), ["uint256"] * 3)


def test_expression_calldata_fields(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "expression_calldata_fields", via_ir, slot_layout, profile)
    item = "(uint256,bool,int16,bytes3,address,int8,uint8)"
    first = (1, False, 1, b"abc", "0x" + "00" * 20, 1, 0)
    second = (7, True, -129, b"xyz", "0x" + "00" * 18 + "1234", -7, 1)
    if profile == "arc4":
        # ARC4 accounts are 32 bytes; the EVM input below remains a 20-byte address.
        first = (*first[:4], bytes(32), *first[5:])
        second = (*second[:4], (0x1234).to_bytes(32, "big"), *second[5:])
    check(f"repoint({item},{item})", [first, second], (7, True, -129, b"xyz", True, -7, 1),
          ["uint256", "bool", "int256", "bytes3", "bool", "int256", "uint256"], [item] * 2)
    for pointer in (4096, 2**64, 2**200):
        check(f"zeroPadded({item},uint256)", [first, pointer], (0, False),
              ["uint256", "bool"], [item, "uint256"])
    # Repoint the bool field onto a signed integer: it must validate, not
    # silently turn arbitrary nonzero bytes into true.
    check(f"zeroPadded({item},uint256)", [second, 36], None,
          ["uint256", "bool"], [item, "uint256"])
    # Struct field address arithmetic wraps at 256 bits, before range checks.
    check(f"zeroPadded({item},uint256)", [first, 2**256 - 32], None,
          ["uint256", "bool"], [item, "uint256"])
