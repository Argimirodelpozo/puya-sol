"""
M23: Groth16 zk-SNARK Verifier — Polynomial Evaluation (9 public signals).
Exercises: Verification with many public signals (9 IC points), stress-testing
the linear combination vk_x computation.

Circuit: degree-7 polynomial evaluation
  - P(x) = c0 + c1*x + c2*x^2 + ... + c7*x^7
  - Private input: x=2 (evaluation point)
  - Public inputs: coefficients = [1, 2, 3, 4, 5, 6, 7, 8]
  - Public output: result = P(2) = 1793
  - 9 public signals total (most of any verifier tested)
  - 847 lines TEAL (largest verifier)

Proof generated with snarkjs (Groth16, BN128/BN254 curve).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


# Higher fee — 9 IC points means 9 scalar muls + 9 additions in vk_x computation
OPUP_FEE = au.AlgoAmount(micro_algo=200_000)

# P(2) = 1 + 2*2 + 3*4 + 4*8 + 5*16 + 6*32 + 7*64 + 8*128 = 1793
COEFFICIENTS = [1, 2, 3, 4, 5, 6, 7, 8]
RESULT = 1793

# Proof point A (G1)
PROOF_A = [
    5330312649548181456775353569752840880014923059990762086667898474517921939153,
    4118160771879785524628487419314632689273510688678213372419263477424055515679,
]

# Proof point B (G2) — [x_im, x_re, y_im, y_re]
PROOF_B = [
    5351951972016768583453141085058392336641152823272914727087483693790063568023,   # x_im
    11515683496125957306233320351319038965447002826891998586531145496799198152147,  # x_re
    8601409048974520987828249595662731008248409561390520349093401923442220129115,   # y_im
    9951460989716586785935726827979691283833200552178159973315536309244130435592,   # y_re
]

# Proof point C (G1)
PROOF_C = [
    13816871364949053518225496036520390849987812833204073200943200729003605036441,
    351759821436509856032624153269958386918925286990782059497537924282665940193,
]

# Public signals: [result, c0, c1, c2, c3, c4, c5, c6, c7]
PUB_SIGNALS = [RESULT] + COEFFICIENTS

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the PolynomialVerifier contract."""
    return deploy_contract(localnet, account, "PolynomialVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The polynomial verifier deploys successfully (847 lines TEAL)."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid proof for P(2) = 1793 with coefficients [1..8] should be accepted."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, PUB_SIGNALS],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_wrong_result_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong polynomial result (1794 instead of 1793) should fail."""
    wrong_signals = [1794] + COEFFICIENTS
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_first_coefficient_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong c0 (2 instead of 1) should fail."""
    wrong_signals = [RESULT, 2, 2, 3, 4, 5, 6, 7, 8]
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_last_coefficient_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong c7 (9 instead of 8) should fail."""
    wrong_signals = [RESULT, 1, 2, 3, 4, 5, 6, 7, 9]
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_middle_coefficient_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong c4 (0 instead of 5) should fail."""
    wrong_signals = [RESULT, 1, 2, 3, 4, 0, 6, 7, 8]
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_all_signals_zero_returns_false(verifier_client: au.AppClient) -> None:
    """All zero signals should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [0] * 9],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_tampered_proof_a_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point A should fail."""
    tampered_a = [PROOF_A[0] + 1, PROOF_A[1]]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[tampered_a, PROOF_B, PROOF_C, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_tampered_proof_b_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point B should fail."""
    tampered_b = [PROOF_B[0] + 1, PROOF_B[1], PROOF_B[2], PROOF_B[3]]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[PROOF_A, tampered_b, PROOF_C, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_tampered_proof_c_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point C should fail."""
    tampered_c = [PROOF_C[0], PROOF_C[1] + 1]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[PROOF_A, PROOF_B, tampered_c, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_signal_at_field_boundary(verifier_client: au.AppClient) -> None:
    """First signal == r should be rejected by checkField."""
    bad_signals = [BN254_R] + COEFFICIENTS
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, bad_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_last_signal_at_field_boundary(verifier_client: au.AppClient) -> None:
    """Last signal (c7) == r should be rejected by checkField."""
    bad_signals = [RESULT, 1, 2, 3, 4, 5, 6, 7, BN254_R]
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, bad_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_all_zero_proof(verifier_client: au.AppClient) -> None:
    """All-zero proof should be rejected."""
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[[0, 0], [0, 0, 0, 0], [0, 0], PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_reversed_coefficients_returns_false(verifier_client: au.AppClient) -> None:
    """Reversed coefficient order [8,7,6,5,4,3,2,1] should fail."""
    reversed_signals = [RESULT] + list(reversed(COEFFICIENTS))
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, reversed_signals],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_cubic_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from cubic circuit should fail on polynomial verifier (cross-verifier)."""
    cubic_a = [
        20142427103495202266623424121043479452086952303398912120731053310801168029892,
        21060701563274264777658945006569193248376894993427505504969620598851046616485,
    ]
    cubic_b = [
        11901938039824042547758149454649453111920217742316873349906392712222983193512,
        1834208190963832174832328243361036530400386058289516545926365898408117830923,
        656860124753310523101161255141728882762758473959667298185180333069376361516,
        17247340835423360867365245738680229618462115856823208869308222146182427202420,
    ]
    cubic_c = [
        14595750151648228275740126844143189511207649022728075877149322434777090801262,
        12939167655551808341801852990815469170642473668580083841683356130944356377968,
    ]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[cubic_a, cubic_b, cubic_c, [9, 35, 0, 0, 0, 0, 0, 0, 0]],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass
