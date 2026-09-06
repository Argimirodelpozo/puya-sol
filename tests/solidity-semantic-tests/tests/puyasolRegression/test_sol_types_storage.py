"""Solc-led correctness regressions for the rev-2 type/storage changes."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_bytes, as_int, as_signed_int
from framework.compile import CompileError


def _call(harness, app, profile, signature, returns, args=(), types=(), *, extra_fee=20_000):
    if profile == "arc4":
        result = harness.call(app, signature, *args, extra_fee=extra_fee)
        assert not result.reverted, result.fail_message
        return (result.abi_return,) if len(returns) == 1 else result.abi_return
    selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
    result = harness.call_raw(app, selector, extra_args=(encode(types, args),), extra_fee=extra_fee)
    assert not result.reverted, result.fail_message
    assert result.logs[-1].startswith(bytes.fromhex("151f7c75"))
    return decode(returns, result.logs[-1][4:])


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_typed_constant_words(harness, via_ir, abi):
    # Canonical solc words: fixed bytes widen on the right; literal text is
    # UTF-8 even when its contents begin with a numeric-looking prefix.
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_constants.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", abi],
    )
    expected = tuple(value.ljust(32, b"\0") for value in (
        b"\x12", b"0x12", bytes.fromhex("0030781200"), bytes.fromhex("12345678"),
    ))
    for method in ("high()", "words()"):
        result = _call(harness, app, abi, method, ["bytes32"] * 4)
        assert tuple(map(as_bytes, result)) == expected
    result = _call(harness, app, abi, "scalars()", ["uint256", "int256", "bool", "uint256"])
    assert (as_int(result[0]), as_signed_int(result[1]), result[2], as_int(result[3])) == (
        2, -7, True, int("12" * 20, 16),
    )


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("contract", ["TransientSchema", "MixedTransientSchema"])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_transient_exact_deployment_schema(harness, slot_layout, contract, abi):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_transient_schema.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )
    app = harness.deploy(artifacts, contract, exact_schema=True)
    schema = app.app_spec.state.schema.global_state
    if contract == "TransientSchema":
        assert (schema.ints, schema.bytes) == (0, 0)
    else:
        assert as_int(_call(harness, app, abi, "persistent()", ["uint64"])[0]) == 17
    result = _call(harness, app, abi, "run()", ["uint128", "uint128", "bool"])
    assert (as_int(result[0]), as_int(result[1]), result[2]) == (11, 23, True)
    assert as_int(_call(harness, app, abi, "value()", ["uint128"])[0]) == 0


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_ir", [False, True])
def test_array_conversion_evaluates_sources_once(harness, slot_layout, abi, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_array_conversions.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_ir,
    )
    for method, expected in (
        ("literalCopy()", (2, -3, -3)),
        ("fixedToDynamic()", (2, -3, -3)),
        ("dynamicToDynamic()", (2, -3, -3)),
        ("tupleCopy()", (4, -3, -3)),
        ("memberCopy()", (2, -3, -3)),
        ("argumentReturn()", (2, -3, -3)),
        ("fixedToFixed()", (2, -3, -3, 0)),
        ("literalToFixed()", (2, -3, -3, 0)),
    ):
        returns = ["uint256", "int16", "int128"] if method == "tupleCopy()" else (
            ["uint256"] + ["int16"] * (len(expected) - 1))
        result = _call(harness, app, abi, method, returns)
        assert (as_int(result[0]), *map(as_signed_int, result[1:])) == expected
    assert as_int(_call(harness, app, abi, "emptyCopy()", ["uint256"])[0]) == 0
    for method, expected in (("initial()", (-7, 9)), ("initialDynamic()", (-3, -3))):
        result = _call(harness, app, abi, method, ["int16", "int16"])
        assert tuple(map(as_signed_int, result)) == expected
    for a, b in ((-(1 << 71), -1), (-7, 9), (0, (1 << 71) - 1)):
        result = _call(harness, app, abi, "wide(int72,int72)", ["int128", "int128"],
                            (a, b), ["int72", "int72"])
        assert tuple(map(as_signed_int, result)) == (a, b)
    for value in (0, 127, 255):
        result = _call(harness, app, abi, "unsignedCopy(uint8)", ["uint16"],
                            (value,), ["uint8"])
        assert as_int(result[0]) == value


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_fixed_array_conversion_loop(harness, slot_layout, abi):
    # 257 elements select the runtime conversion loop, not literal unrolling.
    source = "puyasolRegression/contracts/rev_2_array_conversion_loop.sol"
    extra_args = ["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else [])
    if slot_layout:
        # Slot-mode whole fixed-array stores already have an explicit 64-element
        # capacity limit. Widening must not bypass it or silently truncate.
        with pytest.raises(CompileError, match="unrolled writes capped at 64"):
            harness.compile(source, extra_args=extra_args)
        return
    app = harness.compile_and_deploy(
        source, extra_args=extra_args,
        ensure_budget={"run": 100_000},
    )
    result = _call(harness, app, abi, "run()", ["uint256"] + ["int16"] * 4,
                   extra_fee=200_000)
    assert (as_int(result[0]), *map(as_signed_int, result[1:])) == (2, -3, 0, -3, 0)
