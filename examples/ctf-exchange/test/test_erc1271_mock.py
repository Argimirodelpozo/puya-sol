"""Tests for the ERC1271Mock contract-wallet signature validator.

Mirrors the spirit of ERC1271Signature.t.sol — validates the contract-wallet
signature dispatch path. Full sig flows in CTFExchange are out of scope here
(v1 CTFExchange doesn't fit on AVM).
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from conftest import AUTO_POPULATE


def test_deploy_succeeds(erc1271_mock):
    assert erc1271_mock.app_id > 0


def test_signer_set_at_creation(erc1271_mock, admin):
    """Constructor stored admin's address as signer (read at create time
    from ApplicationArgs[0])."""
    res = erc1271_mock.send.call(au.AppClientMethodCallParams(
        method="signer", args=[],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)
    # ABI `address` decodes to the base32 string form
    assert res.abi_return == admin.address


def test_invalid_signature_reverts(erc1271_mock):
    """ECDSA.recover throws on malformed signatures; the contract surfaces it
    as a revert rather than the 0x00000000 magic. Matches Solidity's
    'invalid signature length' / 'invalid signature s value' behavior."""
    fake_hash = b"\x01" * 32
    fake_sig = b"\x02" * 65
    with pytest.raises(LogicError):
        erc1271_mock.send.call(au.AppClientMethodCallParams(
            method="isValidSignature", args=[fake_hash, fake_sig],
            extra_fee=au.AlgoAmount(micro_algo=10_000),
        ), send_params=AUTO_POPULATE)
