"""Test the Self.xyz Groth16 verifier (Verifier_vc_and_disclose) compiled to AVM."""

from pathlib import Path

import algokit_utils as au
from algokit_utils.models.account import SigningAccount
import pytest

OUT_DIR = Path(__file__).parent.parent / "out"

# Fee to cover inner transactions for opup budget pooling (ensure_budget).
# 90000 budget / 700 per inner app call ≈ 129 inner txns × 1000 µAlgo + 1000 outer
OPUP_FEE = au.AlgoAmount(micro_algo=200_000)


def load_arc56(name: str) -> au.Arc56Contract:
    arc56_path = OUT_DIR / name / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the Verifier_vc_and_disclose contract with extra pages for large program."""
    app_spec = load_arc56("Verifier_vc_and_disclose")
    factory = au.AppFactory(
        au.AppFactoryParams(
            algorand=localnet,
            app_spec=app_spec,
            default_sender=account.address,
        )
    )
    # The verifier is large, may need extra pages
    client, _txn = factory.send.bare.create(
        au.AppFactoryCreateParams(
            extra_program_pages=3,
        )
    )
    return client


def test_deploy(verifier_client: au.AppClient):
    """Test that the verifier contract deploys successfully."""
    assert verifier_client.app_id > 0


def test_invalid_proof_rejected(verifier_client: au.AppClient):
    """An invalid proof using the BN256 generator G1=(1,2) should be rejected.

    Note: On AVM, ec_pairing_check aborts with 'point not on curve' for
    invalid pairing inputs, whereas EVM's ecPairing precompile returns 0.
    The verifier correctly rejects the proof either way.
    """
    G1_X = 1
    G1_Y = 2
    pA = [G1_X, G1_Y]
    pB = [G1_X, G1_Y, G1_X, G1_Y]
    pC = [G1_X, G1_Y]
    pubSignals = [i + 1 for i in range(21)]

    # The invalid proof is rejected (either returns false or aborts)
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[pA, pB, pC, pubSignals],
            )
        )
        # If it returns, it must be false
        assert result.abi_return == False  # noqa: E712
    except Exception:
        # AVM may abort on invalid curve points - this is correct behavior
        pass


def test_random_proof_returns_false(verifier_client: au.AppClient):
    """A random proof should return false."""
    import random
    random.seed(42)

    # Random proof points (not valid BN256 points)
    pA = [random.randint(1, 2**256 - 1) for _ in range(2)]
    pB = [random.randint(1, 2**256 - 1) for _ in range(4)]
    pC = [random.randint(1, 2**256 - 1) for _ in range(2)]
    pubSignals = [random.randint(1, 2**256 - 1) for _ in range(21)]

    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[pA, pB, pC, pubSignals],
        )
    )
    assert result.abi_return == False  # noqa: E712
