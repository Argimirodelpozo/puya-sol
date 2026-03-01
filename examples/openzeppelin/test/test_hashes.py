"""
OpenZeppelin Hashes behavioral tests.
Tests commutative and efficient keccak256.
"""

import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def hashes(localnet, account):
    return deploy_contract(localnet, account, "HashesTest")


def keccak256(data: bytes) -> bytes:
    """Python keccak256 for reference."""
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


def abi_bytes(ret) -> bytes:
    """Convert ABI return (list of ints for byte[N]) to bytes."""
    if isinstance(ret, (list, tuple)):
        return bytes(ret)
    return ret


def test_deploy(hashes):
    assert hashes.app_id > 0


def test_efficient_keccak256(hashes):
    a = b"\x01" * 32
    b = b"\x02" * 32
    result = hashes.send.call(
        au.AppClientMethodCallParams(
            method="efficientKeccak256",
            args=[a, b],
        )
    )
    expected = keccak256(a + b)
    assert abi_bytes(result.abi_return) == expected


def test_commutative_keccak256_same_order(hashes):
    a = b"\x01" * 32
    b = b"\x02" * 32
    result = hashes.send.call(
        au.AppClientMethodCallParams(
            method="commutativeKeccak256",
            args=[a, b],
        )
    )
    # a < b, so commutative(a, b) = efficient(a, b)
    expected = keccak256(a + b)
    assert abi_bytes(result.abi_return) == expected


def test_commutative_keccak256_reversed(hashes):
    a = b"\x01" * 32
    b = b"\x02" * 32
    # commutative(b, a) should equal commutative(a, b)
    result1 = hashes.send.call(
        au.AppClientMethodCallParams(
            method="commutativeKeccak256",
            args=[a, b],
        )
    )
    result2 = hashes.send.call(
        au.AppClientMethodCallParams(
            method="commutativeKeccak256",
            args=[b, a],
        )
    )
    assert result1.abi_return == result2.abi_return


def test_commutative_keccak256_equal_inputs(hashes):
    a = b"\xab" * 32
    result = hashes.send.call(
        au.AppClientMethodCallParams(
            method="commutativeKeccak256",
            args=[a, a],
        )
    )
    expected = keccak256(a + a)
    assert abi_bytes(result.abi_return) == expected


def test_efficient_keccak256_zero_inputs(hashes):
    a = b"\x00" * 32
    b = b"\x00" * 32
    result = hashes.send.call(
        au.AppClientMethodCallParams(
            method="efficientKeccak256",
            args=[a, b],
        )
    )
    expected = keccak256(a + b)
    assert abi_bytes(result.abi_return) == expected
