"""
M17: Groth16 zk-SNARK Verifier end-to-end test.
Exercises: BN254 elliptic curve operations (ec_scalar_mul, ec_add, ec_pairing_check),
large assembly blocks with many constants, calldataload mapping, and the complete
snarkJS Groth16 verifier compiled from Solidity to TEAL.

Uses opup budget pooling (ensure_budget) for sufficient opcode budget (~90,000).
Inner transactions with Fee=0 draw from the group's pooled fee credit, so the
outer call sets a static_fee high enough to cover all inner txn fees.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


# BN254 field modulus (q) and scalar field order (r)
BN254_Q = 21888242871839275222246405745257275088696311157297823662689037894645226208583
BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617

# BN254 G1 generator point (on the curve: 2^2 = 1^3 + 3)
G1_X = 1
G1_Y = 2

# BN254 G2 generator point (standard coordinates used by Ethereum precompile)
G2_X1 = 11559732032986387107991004021392285783925812861821192530917403151452391805634
G2_X2 = 10857046999023057135944570762232829481370756359578518086990519993285655852781
G2_Y1 = 4082367875863433681332203403145435568316851327593401208105741076214120093531
G2_Y2 = 8495653923123431417604973247489272438418190587263600148770280649306958101930

# Fee to cover inner transactions for opup budget pooling.
# 90000 budget / 700 per inner app call ≈ 129 inner txns × 1000 µAlgo + 1000 outer = 130,000
OPUP_FEE = au.AlgoAmount(micro_algo=200_000)


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the Groth16 Verifier_vc_and_disclose contract.

    The approval program is ~4500 bytes, requiring extra_pages=2.
    """
    client = deploy_contract(
        localnet, account, "Verifier_vc_and_disclose", extra_pages=2
    )
    return client


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The Groth16 verifier contract deploys successfully with BN254 opcodes."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_invalid_proof_returns_false(verifier_client: au.AppClient) -> None:
    """verifyProof with valid curve points but invalid proof should return false."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[
                [G1_X, G1_Y],                           # _pA: G1 generator
                [G2_X1, G2_X2, G2_Y1, G2_Y2],           # _pB: G2 generator
                [G1_X, G1_Y],                            # _pC: G1 generator
                [0] * 21,                                # _pubSignals: all zeros
            ],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_signal_out_of_range_returns_false(verifier_client: au.AppClient) -> None:
    """verifyProof with a public signal >= r (scalar field order) should return false
    because checkField rejects it."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[
                [G1_X, G1_Y],                           # _pA: valid G1 point
                [G2_X1, G2_X2, G2_Y1, G2_Y2],           # _pB: valid G2 point
                [G1_X, G1_Y],                            # _pC: valid G1 point
                [BN254_R] + [0] * 20,                    # first signal = r (>= r!)
            ],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_different_invalid_proof_returns_false(verifier_client: au.AppClient) -> None:
    """verifyProof with another set of valid-but-wrong proof data returns false."""
    # Use 2*G1 = (x, y) where we compute the point doubling
    # For BN254 G1, 2*G1 has known coordinates:
    g1_2x = 1368015179489954701390400359078579693043519447331113978918064868415326638035
    g1_2y = 9918110051302171585080402603319702774565515993150576347155970296011118125764
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[
                [g1_2x, g1_2y],                         # _pA: 2*G1
                [G2_X1, G2_X2, G2_Y1, G2_Y2],           # _pB: G2 generator
                [g1_2x, g1_2y],                          # _pC: 2*G1
                [1] * 21,                                # _pubSignals: all ones
            ],
        )
    )
    assert result.abi_return is False
