"""Probe: can we sign with eth_account and have helper1's ECDSA.recover
recover the matching eth address on AVM?

If this works, the EOA signing path is unlocked for all signature tests
that don't depend on the orchestrator's hashOrder (since we can compute
the hash off-chain in Python).
"""
import algokit_utils as au
import pytest

from conftest import AUTO_POPULATE


def test_ecdsa_recover_matches_eth_account(helper1):
    """ECDSA.recover(hash, sig) should return the eth address that signed.

    eth_account signs via secp256k1, AVM has ecdsa_pk_recover for the same
    curve, so the recovered pubkey → keccak256 → last-20-bytes should equal
    the signer's eth address.
    """
    try:
        from eth_account import Account
        from eth_keys import keys
    except ImportError:
        pytest.skip("eth_account / eth_keys not installed")

    acct = Account.create()
    eth_addr = bytes.fromhex(acct.address[2:].lower())  # 20 bytes
    assert len(eth_addr) == 20

    # Arbitrary 32-byte hash to sign.
    msg_hash = b"\x42" * 32

    # eth_account signs the prefixed hash by default. ECDSA.recover in
    # Solidity expects a raw signature over the (already-hashed) message.
    # Use Account.signHash / unsafe_sign_hash for raw signature.
    signed = Account._sign_hash(msg_hash, acct.key)
    # signed.r, signed.s, signed.v
    r = signed.r.to_bytes(32, "big")
    s = signed.s.to_bytes(32, "big")
    v = signed.v.to_bytes(1, "big")
    sig_65 = r + s + v
    assert len(sig_65) == 65

    # Call helper1 ECDSA.recover(byte[32] hash, byte[] signature) -> address.
    # algokit decodes byte[32] from list[int] and byte[] from bytes.
    res = helper1.send.call(au.AppClientMethodCallParams(
        method="ECDSA.recover",
        args=[list(msg_hash), list(sig_65)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
    ), send_params=AUTO_POPULATE)
    recovered = res.abi_return  # Solidity-side address; expect 32-byte form.

    # Algokit decodes `address` to a 58-char base32 algo address. Convert
    # to raw 32 bytes to compare against eth_addr (in lower 20).
    from algosdk import encoding as algosdk_encoding
    recovered_bytes = algosdk_encoding.decode_address(recovered)
    assert len(recovered_bytes) == 32

    # Solidity's address is 20 bytes; puya-sol emits it as 32 bytes
    # zero-padded on the LEFT (high bytes zero, low 20 bytes = eth addr).
    # Or zero-padded on the RIGHT? Let's print and assert based on observed
    # layout.
    print(f"eth addr     = {eth_addr.hex()}")
    print(f"recovered    = {recovered_bytes.hex()}")

    # Try both layouts
    if recovered_bytes[:20] == eth_addr:
        print("layout: eth_addr in low 20 bytes")
    elif recovered_bytes[12:] == eth_addr:
        print("layout: eth_addr in high 20 bytes (left-padded)")
    elif recovered_bytes[-20:] == eth_addr:
        print("layout: eth_addr in last 20 bytes")
    else:
        pytest.fail(f"recovered {recovered_bytes.hex()} doesn't match "
                    f"eth_addr {eth_addr.hex()} in any expected layout")
