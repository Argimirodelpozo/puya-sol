"""Typed values must not replace writable places; enum checks retain full words."""

import pytest

from test_ast_audit import compile_app
from test_call_operands import invoke
from test_lazy_fixed_boxes import _call


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_enum_array_places(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "eb_value_boundaries", via_ir, profile, slot)
    app = harness.deploy(artifacts, "EnumArrayPlaces", fund_wei=30_000_000)
    for signature, args, expected in (
        ("memoryWrite()", [], (1,)),
        ("parameterWrite(uint8[2])", [[0, 2]], (12,)),
        ("dynamicWrite(uint8[])", [[0, 1]], (2,)),
        ("tupleWrite()", [], (12,)),
        ("nestedWrite()", [], (2,)),
        ("storageReference()", [], (1,)),
        ("deleteElement()", [], (1,)),
        ("storageWrite()", [], (1, 2)),
        ("evaluateOnce()", [], (1, 2)),
    ):
        assert invoke(harness, app, profile, signature, args,
                      ["uint256"] * len(expected)) == expected
    invoke(harness, app, profile, "dynamicWrite(uint8[])", [[]], reverts=True)
    for word in (0, 1, 2, 3, 2**64, 2**256 - 1):
        invalid = word >= 3
        for name in ("explicitReturn", "implicitReturn"):
            result = invoke(harness, app, profile, name + "(uint256)", [word],
                            ["uint8"], reverts=invalid)
            if not invalid:
                assert result == (word,)
        result = invoke(harness, app, profile, "castDiscard(uint256)", [word], reverts=invalid)
        if not invalid:
            assert result == (7,)
        for action in range(4):
            result = invoke(harness, app, profile, "dirty(uint256,uint256)",
                            [word, action], reverts=invalid)
            if not invalid:
                assert result == ((word, int(word != 0), word, 7)[action],)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_named_array_lengths(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "eb_value_boundaries", via_ir, profile, slot)
    app = harness.deploy(artifacts, "NamedArrayLengths", fund_wei=30_000_000)

    def lengths(n):
        assert invoke(harness, app, profile, "lengths()", returns=["uint256"] * 4) == (n,) * 4

    lengths(0)
    for n in range(1, 10):
        _call(harness, app, profile, "push()")
        lengths(n)
    _call(harness, app, profile, "pop()")
    lengths(8)
    _call(harness, app, profile, "clear()")
    lengths(0)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_value_comparisons(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "eb_value_boundaries", via_ir, profile, slot)
    app = harness.deploy(artifacts, "ValueComparisons")

    def mask(a, b):
        return sum(int(v) << i for i, v in enumerate((a == b, a != b, a < b, a <= b, a > b, a >= b)))

    for a, b in ((b"ab", b"ab\x00\x00"), (b"b\x00", b"aaaa"), (b"\x00\x00", b"\xff" * 4)):
        assert invoke(harness, app, profile, "fixedBytes(bytes2,bytes4)", [a, b]) == (mask(a + bytes(2), b),)
    for name, bits in (("narrow", 64), ("wide", 128)):
        for a, b in ((0, 0), (-1, 0), (0, -1), (-(2**(bits - 1)), 2**(bits - 1) - 1)):
            assert invoke(harness, app, profile, f"{name}(int{bits},int{bits})", [a, b]) == (mask(a, b),)
    for n in (0, 1, 2**159 + 17, 2**160 - 1):
        for b in (False, True):
            raw = (2**160 - 1 - n).to_bytes(20, "big")
            assert invoke(harness, app, profile, "converted(bytes20,uint160,bool)", [raw, n, b],
                          ["bytes20", "bytes20", "bool"]) == (raw, n.to_bytes(20, "big"), b)
