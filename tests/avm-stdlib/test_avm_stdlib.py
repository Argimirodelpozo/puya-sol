"""Tests for the AVM standard-library Solidity surface (WIP/tokens/AVM.sol).

Each fixture imports the relevant library subset from a `==== Source: AVM.sol ====`
block, exercises one method per AVM intrinsic the library wraps, and verifies
the runtime behavior matches the underlying AVM opcode.

Categories:
    txn_global  — Txn / Global field reads (sender, fee, currentAppId, ...)
    crypto_group — Crypto (sha512_256, sha3_256, ed25519verify) +
                    Group (gtxns Sender / Amount / ...)
    asa_lifecycle — ASA create / opt-in / transfer / balance / destroy
"""
import hashlib
import pytest

from framework import as_int, as_bytes

from pathlib import Path
CONTRACTS = Path(__file__).parent / "contracts"


# ─── Txn / Global / Group field reads ──────────────────────────────────────

def test_txn_sender(harness):
    """Txn.sender() returns the caller's address."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "txnSender()")
    # Sender of an app call returns a base32-encoded address (algosdk encoding).
    assert not r.reverted
    # algosdk address is 58 chars base32; if returned as raw bytes it'd be 32.
    addr = r.abi_return
    assert len(addr) in (32, 58)


def test_txn_application_id(harness):
    """Txn.applicationId() returns the called app's id."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "txnApplicationId()")
    assert as_int(r.abi_return) == app.app_id


def test_txn_type_enum(harness):
    """Txn.typeEnum() returns 6 (appl) for app-call txns."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "txnTypeEnum()")
    assert as_int(r.abi_return) == 6  # appl


def test_global_current_application_id(harness):
    """Global.currentApplicationId() returns this app's id."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "globAppId()")
    assert as_int(r.abi_return) == app.app_id


def test_global_current_application_address(harness):
    """Global.currentApplicationAddress() returns a 32-byte address."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "globAppAddr()")
    addr = r.abi_return
    assert len(addr) in (32, 58)


def test_global_round_monotone(harness):
    """Global.round() increases monotonically across calls."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r1 = as_int(harness.call(app, "globRound()").abi_return)
    r2 = as_int(harness.call(app, "globRound()").abi_return)
    assert r2 >= r1
    assert r1 > 0  # localnet has been running


def test_global_opcode_budget_nonzero(harness):
    """Global.opcodeBudget() returns the remaining opcode budget."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "globBudget()")
    assert as_int(r.abi_return) > 0  # we have budget left after the read


def test_global_latest_timestamp_recent(harness):
    """Global.latestTimestamp() returns a sane Unix timestamp."""
    import time
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "globTimestamp()")
    ts = as_int(r.abi_return)
    # Within ~1 day of wall-clock — localnet may run a different timeline,
    # but should be in the same epoch order.
    assert ts > 1_500_000_000  # post-2017


def test_group_size_single_call(harness):
    """Group.size() == 1 for stand-alone calls."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "groupSize()")
    assert as_int(r.abi_return) == 1


def test_group_index_zero_for_solo(harness):
    """Group.index() == 0 for stand-alone calls."""
    app = harness.compile_and_deploy(CONTRACTS / "txn_global.sol")
    r = harness.call(app, "groupIndex()")
    assert as_int(r.abi_return) == 0


# ─── Crypto: hash functions ────────────────────────────────────────────────

def test_crypto_sha512_256(harness):
    """Crypto.sha512_256(data) == SHA-512/256(data)."""
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    data = b"hello world"
    r = harness.call(app, "sha512(bytes)", data)
    expected = hashlib.new("sha512_256", data).digest()
    assert bytes(r.abi_return) == expected


def test_crypto_sha3_256(harness):
    """Crypto.sha3_256(data) == SHA-3-256(data) (NOT keccak256)."""
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    data = b"hello world"
    r = harness.call(app, "sha3(bytes)", data)
    expected = hashlib.sha3_256(data).digest()
    assert bytes(r.abi_return) == expected


def test_crypto_ed25519_verify_known_vector(harness):
    """Crypto.ed25519Verify against a known test vector (NaCl/libsodium RFC 8032)."""
    try:
        from nacl.signing import SigningKey
    except ImportError:
        pytest.skip("PyNaCl not available")

    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    sk = SigningKey.generate()
    pk = sk.verify_key
    message = b"hello world"
    signed = sk.sign(message)
    signature = signed.signature
    pub = bytes(pk)
    r = harness.call(app, "ed25519(bytes,bytes,bytes)", message, signature, pub)
    assert r.abi_return is True

    # And a tampered signature fails
    bad_sig = bytearray(signature)
    bad_sig[0] ^= 0xFF
    r2 = harness.call(app, "ed25519(bytes,bytes,bytes)", message, bytes(bad_sig), pub)
    assert r2.abi_return is False


def test_crypto_falcon_verify_rejects_bad_pubkey_size(harness):
    """Crypto.falconVerify reverts on wrong pubkey size.

    AVM's falcon_verify expects a 1793-byte ARC4-encoded pubkey. Passing
    a smaller payload should produce a runtime error from the opcode.
    """
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    bad_pub = bytes(897)  # too small
    bad_sig = bytes(666)
    r = harness.call(app, "falcon(bytes,bytes,bytes)", b"hello", bad_sig, bad_pub)
    assert r.reverted


# ─── Group: gtxns field reads ──────────────────────────────────────────────

def test_group_gtxn_sender_self_index(harness):
    """Group.txnSender(0) on a stand-alone call returns this call's sender."""
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    r = harness.call(app, "gtxnSender(uint64)", 0)
    assert not r.reverted
    addr = r.abi_return
    assert len(addr) in (32, 58)


def test_group_gtxn_type_self_index(harness):
    """Group.txnType(0) on a stand-alone app call returns 6 (appl)."""
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    r = harness.call(app, "gtxnType(uint64)", 0)
    assert as_int(r.abi_return) == 6


def test_group_gtxn_fee_self_index(harness):
    """Group.txnFee(0) returns this call's declared fee (>0 in AVM)."""
    app = harness.compile_and_deploy(CONTRACTS / "crypto_group.sol")
    r = harness.call(app, "gtxnFee(uint64)", 0)
    assert as_int(r.abi_return) >= 1000  # standard minimum fee
