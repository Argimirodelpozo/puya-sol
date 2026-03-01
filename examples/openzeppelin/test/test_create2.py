"""
Create2 behavioral tests.
Tests deterministic address computation via CREATE2 hash.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def create2(localnet, account):
    return deploy_contract(localnet, account, "Create2Test")


def test_deploy(create2):
    assert create2.app_id > 0


def test_compute_address(create2, account):
    """computeAddress returns a non-zero address."""
    salt = b'\x01' * 32
    bytecode_hash = b'\xaa' * 32
    result = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeAddress",
            args=[salt, bytecode_hash, account.address],
        )
    )
    assert result.abi_return != "" and len(result.abi_return) > 0


def test_compute_address_deterministic(create2, account):
    """Same inputs always produce the same address."""
    salt = b'\x02' * 32
    bytecode_hash = b'\xbb' * 32
    r1 = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeAddress",
            args=[salt, bytecode_hash, account.address],
        )
    )
    r2 = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeAddress",
            args=[salt, bytecode_hash, account.address],
            note=b"repeat",
        )
    )
    assert r1.abi_return == r2.abi_return


def test_different_salt_different_address(create2, account):
    """Different salts produce different addresses."""
    salt1 = b'\x03' * 32
    salt2 = b'\x04' * 32
    bytecode_hash = b'\xcc' * 32
    r1 = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeAddress",
            args=[salt1, bytecode_hash, account.address],
        )
    )
    r2 = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeAddress",
            args=[salt2, bytecode_hash, account.address],
        )
    )
    assert r1.abi_return != r2.abi_return


def test_compute_bytecode_hash(create2):
    """computeBytecodeHash returns a 32-byte keccak256 hash."""
    bytecode = b'\xde\xad\xbe\xef'
    result = create2.send.call(
        au.AppClientMethodCallParams(
            method="computeBytecodeHash",
            args=[bytecode],
        )
    )
    val = bytes(result.abi_return)
    assert len(val) == 32
    assert val != b'\x00' * 32


def test_verify_deterministic(create2, account):
    """verifyDeterministic returns true for same-salt comparison."""
    salt = b'\x05' * 32
    bytecode_hash = b'\xdd' * 32
    result = create2.send.call(
        au.AppClientMethodCallParams(
            method="verifyDeterministic",
            args=[salt, salt, bytecode_hash, account.address],
        )
    )
    assert result.abi_return is True
