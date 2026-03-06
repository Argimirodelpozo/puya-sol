"""
rust-honk — UltraHonk Verifier Tests

Tests the compiled HonkVerifier (plain) and ZKHonkVerifier contracts
using proof fixtures from the miquelcabot/ultrahonk_verifier Rust crate.

Architecture: Option A — orchestrator at both ends of the group.
  Txn 0: orchestrator.verify(proof, pis) → stores args, sets flag, returns true
  Txn 1..N-1: helpers (chain computation via scratch space)
  Txn N: orchestrator.__finish_verify() → reads result, clears flag
"""
import pytest

from helpers import (
    load_proof_bytes, load_public_inputs,
    simulate_verify, corrupt_proof_zeros, corrupt_proof_point,
    resolve_method, simulate_grouped_call,
)
from algosdk.abi import Method
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, AccountTransactionSigner,
)

# --- Constants ---

EXTRA_OPCODE_BUDGET = 320_000

# Sumcheck corruption offsets
PLAIN_SUMCHECK_OFFSET = 0x400          # sumcheck_univariates[0]
ZK_SUMCHECK_OFFSET = 1184              # 0x4A0 decimal
SUMCHECK_CORRUPT_LEN = 32

# Sumcheck v2 corruption (evaluations)
SUMCHECK_V2_OFFSET = 0x2000
SUMCHECK_V2_LEN = 40 * 32             # 40 field elements = 1280 bytes

# Curve point corruption offsets
PLAIN_CURVE_POINT_OFFSET = 0x2500     # GEMINI_FOLD_COMMS(0) for plain
ZK_CURVE_POINT_OFFSET = 0x2940        # LIBRA_COMMITMENTS(1) for ZK

# Pairing corruption offsets
PLAIN_PROOF_SIZE = 14080   # 440 * 32
ZK_PROOF_SIZE = 15712      # 491 * 32
PLAIN_PAIRING_OFFSET = PLAIN_PROOF_SIZE - 2 * 0x80 - 32 * 28  # 12928
ZK_PAIRING_OFFSET = ZK_PROOF_SIZE - 2 * 0x80 - 32 * 4 - 32 * 28  # 14432


# --- Helpers ---

def _get_failure(result):
    """Extract failure message from simulate response."""
    groups = result.simulate_response.get("txn-groups", [])
    if groups:
        msg = groups[0].get("failure-message", "")
        # Extract readable part from the verbose simulate output
        if "logic eval error" in msg:
            return msg[msg.index("logic eval error"):]
        if msg and len(msg) > 300:
            return "..." + msg[-200:]
        return msg if msg else None
    return None


def _assert_verify_false_or_revert(result, desc):
    """Assert the verification either returned False or reverted."""
    failure = _get_failure(result)
    if failure:
        return  # Reverted — acceptable for invalid proofs
    ret = result.abi_results[-1].return_value
    assert ret is False, f"Expected False for {desc}, got {ret}"


# --- Deployment tests ---

class TestDeploy:
    """Test that all verifier contracts deploy successfully."""

    def test_plain_verifier_deployed(self, plain_orchestrator, plain_helpers):
        assert plain_orchestrator.app_id > 0
        assert len(plain_helpers) > 0

    def test_zk_verifier_deployed(self, zk_orchestrator, zk_helpers):
        assert zk_orchestrator.app_id > 0
        assert len(zk_helpers) > 0


# --- Orchestrator mechanism tests ---

class TestOrchestratorMechanism:
    """Test the orchestrator's flag and group mechanism."""

    def test_orchestrator_has_verify_method(self, plain_orchestrator):
        """Orchestrator should have verify() and __finish_verify() methods."""
        methods = {m.name for m in plain_orchestrator.app_spec.methods}
        assert "verify" in methods
        assert "__finish_verify" in methods

    def test_orchestrator_has_auth_method(self, plain_orchestrator):
        """Orchestrator should have __auth__() method."""
        methods = {m.name for m in plain_orchestrator.app_spec.methods}
        assert "__auth__" in methods

    def test_finish_verify_without_flag_reverts(self, plain_orchestrator, algod_client, account):
        """Calling __finish_verify without setting flag should revert."""
        finish_method = resolve_method(plain_orchestrator, "__finish_verify")
        sp = algod_client.suggested_params()
        signer = AccountTransactionSigner(account.private_key)
        atc = AtomicTransactionComposer()
        atc.add_method_call(
            app_id=plain_orchestrator.app_id,
            method=finish_method,
            sender=account.address,
            sp=sp,
            signer=signer,
        )
        from algosdk.v2client.models import SimulateRequest
        sim_request = SimulateRequest(
            txn_groups=[],
            allow_more_logs=True,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
            allow_empty_signatures=True,
            allow_unnamed_resources=True,
        )
        result = atc.simulate(algod_client, sim_request)
        failure = _get_failure(result)
        assert failure is not None, "Expected failure for __finish_verify without flag"

    def test_zk_orchestrator_has_finish_methods(self, zk_orchestrator):
        """ZK orchestrator should also have __finish_verify()."""
        methods = {m.name for m in zk_orchestrator.app_spec.methods}
        assert "verify" in methods
        assert "__finish_verify" in methods


# --- Positive verification tests ---
# NOTE: These tests are currently blocked by ApplicationArgs size limit (2048 bytes).
# The proof is 14080/15712 bytes, which exceeds the limit.
# These will work once proof loading via box storage is implemented.

class TestVerifyValid:
    """Test that valid proofs verify successfully."""

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_verify_valid_plain_proof(self, plain_orchestrator, algod_client, account):
        """Valid plain proof with correct public inputs should verify."""
        proof = load_proof_bytes("plain_proof")
        pis = load_public_inputs()
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        failure = _get_failure(result)
        assert failure is None, f"Simulate failed: {failure}"
        assert result.abi_results[-1].return_value is True

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_verify_valid_zk_proof(self, zk_orchestrator, algod_client, account):
        """Valid ZK proof with correct public inputs should verify."""
        proof = load_proof_bytes("zk_proof")
        pis = load_public_inputs()
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        failure = _get_failure(result)
        assert failure is None, f"Simulate failed: {failure}"
        assert result.abi_results[-1].return_value is True


# --- Public input error tests ---

class TestWrongPublicInputs:
    """Test that wrong number of public inputs is rejected."""

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_plain_proof_wrong_pi_count(self, plain_orchestrator, algod_client, account):
        """Empty public inputs should cause revert."""
        proof = load_proof_bytes("plain_proof")
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, [],
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        assert _get_failure(result), "Expected failure for wrong PI count"

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_zk_proof_wrong_pi_count(self, zk_orchestrator, algod_client, account):
        """Empty public inputs should cause revert."""
        proof = load_proof_bytes("zk_proof")
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, [],
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        assert _get_failure(result), "Expected failure for wrong PI count"


# --- Sumcheck failure tests ---

class TestSumcheckFailure:
    """Test that corrupted sumcheck data causes verification failure."""

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_plain_proof_bad_sumcheck(self, plain_orchestrator, algod_client, account):
        """Zeroed sumcheck_univariates[0] should fail verification."""
        proof = corrupt_proof_zeros(
            load_proof_bytes("plain_proof"),
            PLAIN_SUMCHECK_OFFSET, SUMCHECK_CORRUPT_LEN,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad sumcheck")

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_zk_proof_bad_sumcheck(self, zk_orchestrator, algod_client, account):
        """Zeroed sumcheck data should fail verification."""
        proof = corrupt_proof_zeros(
            load_proof_bytes("zk_proof"),
            ZK_SUMCHECK_OFFSET, SUMCHECK_CORRUPT_LEN,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad sumcheck")

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_plain_proof_bad_sumcheck_v2(self, plain_orchestrator, algod_client, account):
        """Zeroed sumcheck evaluations should fail."""
        proof = corrupt_proof_zeros(
            load_proof_bytes("plain_proof"),
            SUMCHECK_V2_OFFSET, SUMCHECK_V2_LEN,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad sumcheck v2")

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_zk_proof_bad_sumcheck_v2(self, zk_orchestrator, algod_client, account):
        """Zeroed sumcheck evaluations should fail verification."""
        proof = corrupt_proof_zeros(
            load_proof_bytes("zk_proof"),
            SUMCHECK_V2_OFFSET, SUMCHECK_V2_LEN,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad sumcheck v2")


# --- Curve point validation tests ---

class TestCurvePointFailure:
    """Test that invalid curve points cause verification failure."""

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_plain_proof_bad_curve_points(self, plain_orchestrator, algod_client, account):
        """Invalid G1 point (1,3) at GEMINI_FOLD_COMMS(0) should fail."""
        proof = corrupt_proof_point(
            load_proof_bytes("plain_proof"),
            PLAIN_CURVE_POINT_OFFSET,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad curve point")

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_zk_proof_bad_curve_points(self, zk_orchestrator, algod_client, account):
        """Invalid G1 point at LIBRA_COMMITMENTS(1) should fail."""
        proof = corrupt_proof_point(
            load_proof_bytes("zk_proof"),
            ZK_CURVE_POINT_OFFSET,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad curve point")


# --- Pairing check failure tests ---

class TestPairingFailure:
    """Test that corrupted pairing data causes verification failure."""

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_plain_proof_bad_pairing(self, plain_orchestrator, algod_client, account):
        """Corrupted gemini_a_evaluations should fail pairing check."""
        proof = corrupt_proof_point(
            load_proof_bytes("plain_proof"),
            PLAIN_PAIRING_OFFSET,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            plain_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad pairing")

    @pytest.mark.skip(reason="Proof exceeds ApplicationArgs 2048-byte limit (14KB > 2KB)")
    def test_reject_zk_proof_bad_pairing(self, zk_orchestrator, algod_client, account):
        """Corrupted gemini_a_evaluations should fail pairing check."""
        proof = corrupt_proof_point(
            load_proof_bytes("zk_proof"),
            ZK_PAIRING_OFFSET,
        )
        pis = load_public_inputs()
        result = simulate_verify(
            zk_orchestrator, algod_client, account,
            proof, pis,
            extra_opcode_budget=EXTRA_OPCODE_BUDGET,
        )
        _assert_verify_false_or_revert(result, "bad pairing")
