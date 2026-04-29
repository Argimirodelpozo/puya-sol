"""
MerkleProofTest behavioral tests.
Tests Merkle tree root computation and proof verification with fixed-depth proofs.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


def abi_bytes(val) -> bytes:
    """Convert algokit ABI return (list[int]) to bytes."""
    if isinstance(val, (bytes, bytearray)):
        return bytes(val)
    return bytes(val)


def keccak256(*args: bytes) -> bytes:
    """Compute keccak256 hash."""
    from Crypto.Hash import keccak
    h = keccak.new(digest_bits=256)
    for a in args:
        h.update(a)
    return h.digest()


def hash_pair(a: bytes, b: bytes) -> bytes:
    """Hash pair in sorted order (same as MerkleProof._hashPair)."""
    a_int = int.from_bytes(a, "big")
    b_int = int.from_bytes(b, "big")
    if a_int < b_int:
        return keccak256(a + b)
    else:
        return keccak256(b + a)


def make_leaf(i: int) -> bytes:
    """Create a unique 32-byte leaf value."""
    return bytes([i] + [0] * 31)


@pytest.fixture(scope="module")
def merkle(localnet, account):
    return deploy_contract(localnet, account, "MerkleProofTest")


def test_deploy(merkle):
    assert merkle.app_id > 0


def test_hash_pair_ordered(merkle):
    """hashPair should sort inputs before hashing."""
    a = b"\x00" * 31 + b"\x01"
    b_val = b"\x00" * 31 + b"\x02"
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="hashPair",
            args=[a, b_val],
        )
    )
    expected = hash_pair(a, b_val)
    assert abi_bytes(result.abi_return) == expected


def test_hash_pair_reverse_order(merkle):
    """hashPair(b, a) should equal hashPair(a, b) — commutative."""
    a = b"\x00" * 31 + b"\x01"
    b_val = b"\x00" * 31 + b"\x02"
    result1 = merkle.send.call(
        au.AppClientMethodCallParams(
            method="hashPair",
            args=[a, b_val],
        )
    )
    result2 = merkle.send.call(
        au.AppClientMethodCallParams(
            method="hashPair",
            args=[b_val, a],
        )
    )
    assert abi_bytes(result1.abi_return) == abi_bytes(result2.abi_return)


def test_hash_pair_different_inputs(merkle):
    """Different inputs produce different hashes."""
    a = b"\x01" + b"\x00" * 31
    b1 = b"\x02" + b"\x00" * 31
    b2 = b"\x03" + b"\x00" * 31
    r1 = merkle.send.call(
        au.AppClientMethodCallParams(method="hashPair", args=[a, b1])
    )
    r2 = merkle.send.call(
        au.AppClientMethodCallParams(method="hashPair", args=[a, b2])
    )
    assert abi_bytes(r1.abi_return) != abi_bytes(r2.abi_return)


def test_verify_depth1_valid(merkle):
    """Verify a valid depth-1 proof (2-leaf tree)."""
    leaf_a = make_leaf(1)
    leaf_b = make_leaf(2)
    root = hash_pair(leaf_a, leaf_b)

    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth1",
            args=[root, leaf_a, leaf_b],
        )
    )
    assert result.abi_return is True


def test_verify_depth1_invalid(merkle):
    """Invalid root returns false."""
    leaf_a = make_leaf(1)
    leaf_b = make_leaf(2)
    wrong_root = b"\x00" * 32

    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth1",
            args=[wrong_root, leaf_a, leaf_b],
        )
    )
    assert result.abi_return is False


def test_verify_depth1_wrong_proof(merkle):
    """Wrong proof element returns false."""
    leaf_a = make_leaf(1)
    leaf_b = make_leaf(2)
    root = hash_pair(leaf_a, leaf_b)
    wrong_proof = make_leaf(99)

    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth1",
            args=[root, leaf_a, wrong_proof],
        )
    )
    assert result.abi_return is False


def test_verify_depth2_valid(merkle):
    """Verify a valid depth-2 proof (4-leaf tree)."""
    #         root
    #        /    \
    #      h01    h23
    #     /  \   /  \
    #    L0  L1 L2  L3
    l0 = make_leaf(10)
    l1 = make_leaf(20)
    l2 = make_leaf(30)
    l3 = make_leaf(40)
    h01 = hash_pair(l0, l1)
    h23 = hash_pair(l2, l3)
    root = hash_pair(h01, h23)

    # Prove L0: proof = [L1, H23]
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth2",
            args=[root, l0, l1, h23],
        )
    )
    assert result.abi_return is True


def test_verify_depth2_prove_other_leaf(merkle):
    """Prove L3 in a 4-leaf tree."""
    l0 = make_leaf(10)
    l1 = make_leaf(20)
    l2 = make_leaf(30)
    l3 = make_leaf(40)
    h01 = hash_pair(l0, l1)
    h23 = hash_pair(l2, l3)
    root = hash_pair(h01, h23)

    # Prove L3: proof = [L2, H01]
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth2",
            args=[root, l3, l2, h01],
        )
    )
    assert result.abi_return is True


def test_verify_depth2_wrong_leaf(merkle):
    """Wrong leaf fails depth-2 verification."""
    l0 = make_leaf(10)
    l1 = make_leaf(20)
    l2 = make_leaf(30)
    l3 = make_leaf(40)
    h01 = hash_pair(l0, l1)
    h23 = hash_pair(l2, l3)
    root = hash_pair(h01, h23)

    wrong = make_leaf(99)
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth2",
            args=[root, wrong, l1, h23],
        )
    )
    assert result.abi_return is False


def test_verify_depth3_valid(merkle):
    """Verify a valid depth-3 proof (8-leaf tree)."""
    leaves = [make_leaf(i) for i in range(8)]
    h01 = hash_pair(leaves[0], leaves[1])
    h23 = hash_pair(leaves[2], leaves[3])
    h45 = hash_pair(leaves[4], leaves[5])
    h67 = hash_pair(leaves[6], leaves[7])
    h0123 = hash_pair(h01, h23)
    h4567 = hash_pair(h45, h67)
    root = hash_pair(h0123, h4567)

    # Prove leaf[0]: proof = [leaf[1], h23, h4567]
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth3",
            args=[root, leaves[0], leaves[1], h23, h4567],
        )
    )
    assert result.abi_return is True


def test_verify_depth3_prove_inner_leaf(merkle):
    """Prove leaf[5] in an 8-leaf tree."""
    leaves = [make_leaf(i) for i in range(8)]
    h01 = hash_pair(leaves[0], leaves[1])
    h23 = hash_pair(leaves[2], leaves[3])
    h45 = hash_pair(leaves[4], leaves[5])
    h67 = hash_pair(leaves[6], leaves[7])
    h0123 = hash_pair(h01, h23)
    h4567 = hash_pair(h45, h67)
    root = hash_pair(h0123, h4567)

    # Prove leaf[5]: proof = [leaf[4], h67, h0123]
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth3",
            args=[root, leaves[5], leaves[4], h67, h0123],
        )
    )
    assert result.abi_return is True


def test_verify_depth3_invalid(merkle):
    """Wrong proof fails depth-3 verification."""
    leaves = [make_leaf(i) for i in range(8)]
    h01 = hash_pair(leaves[0], leaves[1])
    h23 = hash_pair(leaves[2], leaves[3])
    h45 = hash_pair(leaves[4], leaves[5])
    h67 = hash_pair(leaves[6], leaves[7])
    h0123 = hash_pair(h01, h23)
    h4567 = hash_pair(h45, h67)
    root = hash_pair(h0123, h4567)

    wrong_sibling = make_leaf(99)
    result = merkle.send.call(
        au.AppClientMethodCallParams(
            method="verifyDepth3",
            args=[root, leaves[0], wrong_sibling, h23, h4567],
        )
    )
    assert result.abi_return is False
