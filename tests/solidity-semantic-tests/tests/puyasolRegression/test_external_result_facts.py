"""Both source adapters share byte semantics; every external result replaces the buffer."""

import hashlib

import pytest
from eth_abi import encode
from framework import as_signed_int
from test_assembly_pairing import pairing_cases
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_external_result_facts(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/external_result_facts.sol", via_yul_behavior=via_ir,
        ensure_budget={"pair": 14_000},
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    def check(method, data):
        return invoke(harness, app, profile, method + "(bytes)", [data], ["bytes", "uint256"])
    for data in (b"", b"x", bytes(range(33))):
        assert check("identity", data) == (data, len(data))
        assert check("sha", data) == (hashlib.sha256(data).digest(), 32)
    for size in (0, 31, 127, 128, 129):
        assert check("recovery", bytes(size)) == (b"", 0)
    for method in ("add", "mul"):
        for size in (0, 31, 95, 128, 129):
            assert check(method, bytes(size)) == (bytes(64), 64)
    for data, expected in pairing_cases():
        assert check("pair", data) == (expected.to_bytes(32, "big"), 32)
    for base, exponent, modulus in ((7, 0, 0), (7, 0, 1), (7, 0, 13), (7, 3, 13)):
        data = encode(["uint256"] * 6, [32, 32, 32, base, exponent, modulus])
        expected = pow(base, exponent, modulus) if modulus else 0
        assert check("modexp", data) == (expected.to_bytes(32, "big"), 32)
    empty_size, signed_size, value, calls = invoke(
        harness, app, profile, "transition()", returns=["uint256", "uint256", "int8", "uint64"])
    assert (empty_size, signed_size, as_signed_int(value), calls) == (0, 32, -7, 2)
    expected = encode(["int16", "uint16", "bytes"], [-9, 1234, b"\x12\x34\x56"])
    assert invoke(harness, app, profile, "lowSelf()", returns=["bytes", "uint256", "uint64"]) == (expected, len(expected), 1)
    for first in (False, True):
        size, value, calls = invoke(harness, app, profile, "pointerSelf(bool)", [first], ["uint256", "int8", "uint64"])
        assert (size, as_signed_int(value), calls) == (32, -7 if first else -8, 1)
