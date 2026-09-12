"""Nonzero BN254 pairs exercise coordinate reshaping and dynamic pair counts."""
import pytest
from test_call_operands import invoke


FIELD = 21888242871839275222246405745257275088696311157297823662689037894645226208583
G2_EVM = (
    11559732032986387107991004021392285783925812861821192530917403151452391805634,
    10857046999023057135944570762232829481370756359578518086990519993285655852781,
    4082367875863433681332203403145435568316851327593401208105741076214120093531,
    8495653923123431417604973247489272438418190587263600148770280649306958101930,
)


def encoded_pair(x, y):
    return b"".join(n.to_bytes(32, "big") for n in (x, y, *G2_EVM))


def pairing_cases():
    positive, negative, identity = encoded_pair(1, 2), encoded_pair(1, FIELD - 2), encoded_pair(0, 0)
    return [(b"", 1), (positive, 0), (positive + negative, 1),
            (positive + negative + identity, 1), (positive + negative + positive, 0)]


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_pairing(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/assembly_pairing.sol", via_yul_behavior=via_ir,
        ensure_budget={"pair": 14_000},
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    for data, expected in pairing_cases():
        assert invoke(harness, app, profile, "pair(bytes)", [data]) == (expected,)
