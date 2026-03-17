"""
Tests for the unsplit Verifier contract (single contract, no helpers).

Translated from tornado-core's Verifier validation tests.
Tests the Groth16 verifier's bounds checking, input validation,
and pairing-based proof verification.
"""
import algokit_utils as au
from algokit_utils.models.account import SigningAccount
import pytest

from conftest import deploy_contract


# BN254 field modulus (PRIME_Q in Verifier.sol)
PRIME_Q = 21888242871839275222246405745257275088696311157297823662689037894645226208583

# BN254 scalar field (SNARK_SCALAR_FIELD in Verifier.sol)
SNARK_SCALAR_FIELD = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the unsplit Verifier contract."""
    return deploy_contract(
        localnet, account, "Verifier", subdir="VerifierUnsplit",
        fund_amount=2_000_000, extra_pages=1,
    )


class TestUnsplitVerifierDeployment:
    """Test that the unsplit verifier deploys successfully."""

    def test_deploys(self, verifier: au.AppClient):
        assert verifier.app_id > 0

    def test_verify_proof_valid(self, verifier: au.AppClient):
        """Call verifyProof with a dummy proof — should return a bool (pass bounds checks)."""
        proof_elements = [b'\x00' * 31 + b'\x01'] * 8
        proof = b''.join(proof_elements)
        inputs = [1] * 6

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
            )
        )
        assert isinstance(result.abi_return, bool)

    def test_verify_proof_zero_proof(self, verifier: au.AppClient):
        """Verify that zero proof values pass the PRIME_Q check."""
        proof = b'\x00' * 256
        inputs = [0] * 6

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
            )
        )
        assert isinstance(result.abi_return, bool)


class TestVerifierBoundsChecks:
    """
    Translated from tornado-core Verifier tests.
    Test the require() bounds checks on proof elements and public inputs.
    """

    def test_proof_element_at_prime_q_rejected(self, verifier: au.AppClient):
        """
        Original: 'verifier-proof-element-gte-prime-q'
        A proof element exactly equal to PRIME_Q should be rejected.
        """
        # Put PRIME_Q as the first proof element (A.x)
        bad_element = PRIME_Q.to_bytes(32, "big")
        proof = bad_element + b'\x00' * (256 - 32)  # remaining 7 elements are 0
        inputs = [0] * 6

        with pytest.raises(Exception):
            verifier.send.call(
                au.AppClientMethodCallParams(
                    method="verifyProof",
                    args=[proof, inputs],
                )
            )

    def test_proof_element_above_prime_q_rejected(self, verifier: au.AppClient):
        """A proof element larger than PRIME_Q should be rejected."""
        bad_element = (PRIME_Q + 1).to_bytes(32, "big")
        proof = bad_element + b'\x00' * (256 - 32)
        inputs = [0] * 6

        with pytest.raises(Exception):
            verifier.send.call(
                au.AppClientMethodCallParams(
                    method="verifyProof",
                    args=[proof, inputs],
                )
            )

    def test_proof_element_just_below_prime_q_accepted(self, verifier: au.AppClient):
        """A proof element at PRIME_Q - 1 should pass the bounds check."""
        ok_element = (PRIME_Q - 1).to_bytes(32, "big")
        proof = ok_element + b'\x00' * (256 - 32)
        inputs = [0] * 6

        # Should not revert — bounds check passes (pairing may still return false)
        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
            )
        )
        assert isinstance(result.abi_return, bool)

    def test_input_at_snark_scalar_field_rejected(self, verifier: au.AppClient):
        """
        Original: 'verifier-gte-snark-scalar-field'
        A public input exactly equal to SNARK_SCALAR_FIELD should be rejected.
        """
        proof = b'\x00' * 256
        inputs = [SNARK_SCALAR_FIELD, 0, 0, 0, 0, 0]

        with pytest.raises(Exception):
            verifier.send.call(
                au.AppClientMethodCallParams(
                    method="verifyProof",
                    args=[proof, inputs],
                )
            )

    def test_input_above_snark_scalar_field_rejected(self, verifier: au.AppClient):
        """A public input larger than SNARK_SCALAR_FIELD should be rejected."""
        proof = b'\x00' * 256
        inputs = [0, 0, 0, 0, 0, SNARK_SCALAR_FIELD + 1]

        with pytest.raises(Exception):
            verifier.send.call(
                au.AppClientMethodCallParams(
                    method="verifyProof",
                    args=[proof, inputs],
                )
            )

    def test_input_just_below_snark_scalar_field_accepted(self, verifier: au.AppClient):
        """An input at SNARK_SCALAR_FIELD - 1 should pass the bounds check."""
        proof = b'\x00' * 256
        inputs = [SNARK_SCALAR_FIELD - 1, 0, 0, 0, 0, 0]

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
            )
        )
        assert isinstance(result.abi_return, bool)


class TestVerifierPairingCheck:
    """
    Test the actual Groth16 pairing verification result.
    Without a valid proof from the Tornado Cash circuit, any well-formed
    proof should fail the pairing equation and return false.
    """

    def test_zero_proof_returns_false(self, verifier: au.AppClient):
        """
        Zero proof (point at infinity for all curve points) with zero inputs
        should pass bounds checks but fail the pairing equation.
        """
        proof = b'\x00' * 256
        inputs = [0] * 6

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
                extra_fee=au.AlgoAmount(micro_algo=240 * 1000),
            )
        )
        assert result.abi_return is False

    def test_small_values_proof_returns_false(self, verifier: au.AppClient):
        """
        Proof with small valid values (pass bounds check) but not satisfying
        the Groth16 equation should return false.
        """
        # Use the BN254 generator G1 = (1, 2) as A and C points
        # G2 generator for B
        g1_x = (1).to_bytes(32, "big")
        g1_y = (2).to_bytes(32, "big")
        # G2 generator coordinates
        g2_x0 = (10857046999023057135944570762232829481370756359578518086990519993285655852781).to_bytes(32, "big")
        g2_x1 = (11559732032986387107991004021392285783925812861821192530917403151452391805634).to_bytes(32, "big")
        g2_y0 = (8495653923123431417604973247489272438418190587263600148770280649306958101930).to_bytes(32, "big")
        g2_y1 = (4082367875863433681332203403145435568316851327593401208105741076214120093531).to_bytes(32, "big")

        # proof = [A.x, A.y, B.x[1], B.x[0], B.y[1], B.y[0], C.x, C.y]
        proof = g1_x + g1_y + g2_x1 + g2_x0 + g2_y1 + g2_y0 + g1_x + g1_y
        inputs = [1, 2, 3, 4, 5, 6]

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
                extra_fee=au.AlgoAmount(micro_algo=240 * 1000),
            )
        )
        assert result.abi_return is False

    def test_random_valid_proof_returns_false(self, verifier: au.AppClient):
        """
        A random proof with values within bounds should fail verification.
        The probability of randomly hitting a valid Groth16 proof is negligible.
        """
        import hashlib
        # Generate deterministic but arbitrary proof elements within field
        elements = []
        for i in range(8):
            h = int.from_bytes(hashlib.sha256(f"proof_element_{i}".encode()).digest(), "big")
            elements.append((h % (PRIME_Q - 1)).to_bytes(32, "big"))
        proof = b''.join(elements)

        # Generate deterministic inputs within scalar field
        inputs = []
        for i in range(6):
            h = int.from_bytes(hashlib.sha256(f"input_{i}".encode()).digest(), "big")
            inputs.append(h % (SNARK_SCALAR_FIELD - 1))

        result = verifier.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                args=[proof, inputs],
                extra_fee=au.AlgoAmount(micro_algo=240 * 1000),
            )
        )
        assert result.abi_return is False
