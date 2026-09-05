"""Solc-led correctness regressions for the rev-2 type/storage changes."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode

from framework import as_bytes, as_int, as_signed_int


def _call_zero(harness, app, profile, signature, returns):
    if profile == "arc4":
        result = harness.call(app, signature)
        assert not result.reverted, result.fail_message
        return (result.abi_return,) if len(returns) == 1 else result.abi_return
    selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
    result = harness.call_raw(app, selector, extra_args=(b"",))
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
        result = _call_zero(harness, app, abi, method, ["bytes32"] * 4)
        assert tuple(map(as_bytes, result)) == expected
    result = _call_zero(harness, app, abi, "scalars()", ["uint256", "int256", "bool", "uint256"])
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
        assert as_int(_call_zero(harness, app, abi, "persistent()", ["uint64"])[0]) == 17
    result = _call_zero(harness, app, abi, "run()", ["uint128", "uint128", "bool"])
    assert (as_int(result[0]), as_int(result[1]), result[2]) == (11, 23, True)
    assert as_int(_call_zero(harness, app, abi, "value()", ["uint128"])[0]) == 0
