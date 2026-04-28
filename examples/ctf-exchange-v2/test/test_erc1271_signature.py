"""Translation of v2 src/test/ERC1271Signature.t.sol.

These tests exercise validateOrderSignature with POLY_1271 sigType, which
calls _isValidSignature → _verifyPoly1271Signature → SignatureCheckerLib.
The dispatch lives in helper2 (extracted) but the orch's stub for
_isValidSignature targets helper2 — and validateOrderSignature itself
reads `preapproved` mapping (state on orch). All tests xfail until we
deploy a 1271 mock and wire up the eth-style EOA signing path.
"""
import pytest

pytestmark = pytest.mark.xfail(
    reason="ERC1271 signature path needs an ERC1271 mock + eth-style ECDSA "
           "signing infrastructure on the test side; not yet wired up",
    strict=False,
)


def test_validate_1271_signature(split_exchange):
    """test_ERC1271Signature_validate1271Signature"""
    pytest.fail("ERC1271 signing infrastructure not wired up")


def test_validate_1271_signature_revert_incorrect_signer(split_exchange):
    """test_ERC1271Signature_validate1271Signature_revert_incorrectSigner"""
    pytest.fail("ERC1271 signing infrastructure not wired up")


def test_validate_1271_signature_revert_sig_type(split_exchange):
    """test_ERC1271Signature_validate1271Signature_revert_sigType"""
    pytest.fail("ERC1271 signing infrastructure not wired up")


def test_validate_1271_signature_revert_non_contract(split_exchange):
    """test_ERC1271Signature_validate1271Signature_revert_nonContract"""
    pytest.fail("ERC1271 signing infrastructure not wired up")


def test_validate_1271_signature_revert_invalid_contract(split_exchange):
    """test_ERC1271Signature_validate1271Signature_revert_invalidContract"""
    pytest.fail("ERC1271 signing infrastructure not wired up")


def test_validate_1271_signature_revert_invalid_signer_maker(split_exchange):
    """test_ERC1271Signature_validate1271Signature_revert_invalidSignerMaker"""
    pytest.fail("ERC1271 signing infrastructure not wired up")
