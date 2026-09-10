"""Expression lowering: solc types, assignment values, bounds and scoped effects."""

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


def deploy(harness, source, via_ir, slot_layout, profile):
    app = harness.compile_and_deploy(
        f"puyasolRegression/contracts/expression_{source}.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))

    def check(signature, arguments, expected, returns=None):
        returns = returns or ["uint256"] * (len(expected) if expected is not None else 1)
        fail = expected is None
        if profile == "arc4":
            result = harness.call(app, signature, *arguments, expect_revert=fail)
        else:
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),), expect_revert=fail)
        assert result.reverted == fail, (signature, arguments, result.fail_message)
        if fail:
            return
        if profile == "evm":
            actual = decode(returns, result.logs[-1][4:])
        else:
            values = (result.abi_return,) if len(returns) == 1 else result.abi_return
            actual = tuple(bytes(value) if kind.startswith("bytes") else
                           (as_signed_int(value) if kind.startswith("int") else as_int(value))
                           for kind, value in zip(returns, values, strict=True))
        assert actual == expected, (signature, arguments, actual, expected)

    return check


def test_expression_conversions(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "conversions", via_ir, slot_layout, profile)
    for value in (-128, -7, 0, 127):
        for condition in (False, True):
            check("conditional(bool,int8,int256)", [condition, value, 19],
                  (value if condition else 19,), ["int256"])
            check("tupleConditional(bool,int8,int8)", [condition, value, 19],
                  (value, 19) if condition else (19, value), ["int256"] * 2)
            check("tupleValues(int8,bool)", [value, condition], (value, -2), ["int256"] * 2)
        check("arrayValue(int8)", [value], (value,), ["int256"])
        check("nestedTuple(int8)", [value], (value, value), ["int256"] * 2)
        check("transientValue(int8)", [value], (value, value), ["int256"] * 2)
        check("transientTuple(int8)", [value], (value, -2), ["int256"] * 2)
        check("blobValue(int8)", [value], (value, value), ["int256"] * 2)


def test_expression_power(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "power", via_ir, slot_layout, profile)
    for wrap in (False, True):
        for base, exponent in ((0, 0), (0, 2**64), (1, 2**200), (2, 7), (2, 8), (2, 2**64), (3, 5)):
            expected = pow(base, exponent, 256) if wrap else (pow(base, exponent) if base < 2 or exponent < 10 else 256)
            check("power(uint8,uint256,bool)", [base, exponent, wrap],
                  (expected,) if wrap or expected <= 255 else None)
        for base, exponent in ((-1, 2**64 + 1), (-1, 2**200), (-2, 3), (-2, 7), (-2, 8)):
            expected = pow(base, exponent) if exponent < 10 or base == -1 else 256
            if wrap:
                expected = (expected + 128) % 256 - 128
            check("signedPower(int8,uint256,bool)", [base, exponent, wrap],
                  (expected,) if wrap or -128 <= expected <= 127 else None, ["int256"])


def test_expression_bounds(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "bounds", via_ir, slot_layout, profile)
    for start, end in ((0, 0), (0, 4), (1, 3), (4, 4), (3, 2), (0, 5), (2**64 + 1, 2**64 + 3)):
        valid = start <= end <= 4
        check("slice(bytes,uint256,uint256)", [b"abcd", start, end],
              (b"abcd"[start:end],) if valid else None, ["bytes"])
        check("discardSlice(bytes,uint256,uint256)", [b"abcd", start, end], (7,) if valid else None)
    for index in (0, 1, 2, 2**64, 2**200 + 1):
        for write in (False, True):
            check("blobIndex(uint256,bool)", [index, write],
                  ((77 if write else (42, 55)[index]),) if index < 2 else None)
    for start, end in ((0, 0), (0, 3), (1, 3), (2, 2), (3, 1), (0, 4)):
        valid = start <= end <= 3
        values = [11, 13, 17][start:end]
        check("arraySlice(uint256[],uint256,uint256)", [[11, 13, 17], start, end],
              ((values[0] + values[-1]) if values else 0,) if valid else None)
    check("whole(uint256[])", [[11, 13, 17]], (28,))
    check("whole(uint256[])", [[]], (0,))
    check("boolSlice(bool[],uint256,uint256)", [[True, False, True, False], 1, 3],
          (False, True), ["bool", "bool"])
    for index in (0, 1, 2, 2**64):
        check("nestedSlice(int8[],uint256,uint256,uint256)", [[7, -8, -9], 1, 3, index],
              ((-8, -9)[index],) if index < 2 else None, ["int256"])


def test_expression_assignment_results(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "assignment_results", via_ir, slot_layout, profile)
    for value in (b"", b"abc", b"a" * 33):
        check("blob(bytes)", [value], (value,), ["bytes"])
        check("byteArray(bytes)", [value], (value,), ["bytes"])
    for value in (0, 17, 2**128):
        for method, other in (("structure", 9), ("fixedArray", 11), ("dynamicArray", 13)):
            check(method + "(uint256)", [value], (value, other))
        check("storageReference(uint256)", [value], (value + 1,))
        check("fixedStorageReference(uint256)", [value], (value + 1, value))
        check("reboundReference(uint256)", [value], (8, value))


def test_expression_effects(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "effects", via_ir, slot_layout, profile)
    for compound in (False, True):
        check("blobOrder(bool)", [compound], (9, 24 if compound else 4, 21))
        check("tupleTargets(bool)", [compound], (5, 9, 2))
    check("bytesTargets()", [], (b"\x05", b"\x05\x00", 1), ["bytes1", "bytes", "uint256"])
    for condition in (False, True):
        for disjunction in (False, True):
            mutations = int(not condition) + int(condition != disjunction)
            taken = condition or disjunction
            check("branches(bool,bool)", [condition, disjunction],
                  (3 if condition else 11, mutations * 10 + int(taken)))
    for key in (0, 7):
        check("targets(uint256)", [key], (-7, -5, 3, 0), ["int256", "int256", "uint256", "uint256"])
    for value in (-128, -7, 0, 127):
        check("unary(int8,bytes3)", [value, b"\x01\x02\x03"],
              (-value, b"\xfe\xfd\xfc", bool(value)) if value != -128 else None,
              ["int256", "bytes3", "bool"])
