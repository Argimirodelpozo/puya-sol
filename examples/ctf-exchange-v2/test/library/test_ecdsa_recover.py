"""Verify the puya-sol ECDSA-recover lowering works on real orders.

solc inlines `ECDSA.recover` and `SignatureCheckerLib.isValidSignatureNow`
into their internal callers, so neither survives as a standalone subroutine
that we could call directly via ABI. We exercise the same code path via
the orch's `validateOrderSignature(orderHash, order)` ABI method —
internally it goes through `_verifyECDSASignature` which then uses
solady's ECDSA.recover, all of which puya-sol lowers to AVM's
`ecdsa_pk_recover Secp256k1` opcode (see PrecompileDispatch.cpp::handleEcRecover).

A correctly-signed order should pass; flipping a byte of the signature
should revert with the InvalidSignature-class error. Both halves
prove the recover→address-equality→assert path is wired end-to-end.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from conftest import AUTO_POPULATE
from dev.orders import make_order, sign_order, hash_order_via_contract, Side
from dev.signing import bob


def _send_with_pads(orch, *, target_method, target_args, n_pads=4,
                    extra_fee=80_000):
    """Wrap a single orch ABI call in a small group with `n_pads`
    `isAdmin` no-op pads on the orch so the inner-call opcode pool covers
    the ECDSA recover (~1700 ops)."""
    composer = orch.algorand.new_group()
    for i in range(n_pads):
        composer.add_app_call_method_call(orch.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin", args=[b"\x00" * 32],
                note=f"opup-{i}".encode(),
            )))
    composer.add_app_call_method_call(orch.params.call(
        au.AppClientMethodCallParams(
            method=target_method, args=target_args,
            extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        )))
    return composer.send(au.SendParams(populate_app_call_resources=True))


def test_validateOrderSignature_passes_on_valid_signature(split_exchange_settled):
    """A correctly-signed order's signature validates without revert.
    Exercises the full ECDSA recover path: hash → sig.r/s/v → recover →
    keccak256(pubkey) → low-20-byte address → equality with order.signer."""
    h1, _, orch, _, _ = split_exchange_settled
    bob_signer = bob()
    order = make_order(
        maker=bob_signer.eth_address_padded32, token_id=42,
        maker_amount=1_000, taker_amount=2_000, side=Side.BUY,
    )
    signed = sign_order(orch, order, bob_signer)
    order_hash = hash_order_via_contract(orch, signed)

    _send_with_pads(orch,
        target_method="validateOrderSignature",
        target_args=[list(order_hash), signed.to_abi_list()])


def test_validateOrderSignature_reverts_on_tampered_signature(split_exchange_settled):
    """Flipping one byte of the signature breaks the recover→address-eq
    check; the inner-call should revert with InvalidSignature (or
    similarly-classified error from the signature dispatcher)."""
    h1, _, orch, _, _ = split_exchange_settled
    bob_signer = bob()
    order = make_order(
        maker=bob_signer.eth_address_padded32, token_id=42,
        maker_amount=1_000, taker_amount=2_000, side=Side.BUY,
    )
    signed = sign_order(orch, order, bob_signer)
    order_hash = hash_order_via_contract(orch, signed)

    # Flip a byte in the middle of the 65-byte signature.
    sig = bytearray(signed.signature)
    sig[20] ^= 0xff
    signed.signature = bytes(sig)

    with pytest.raises(LogicError):
        _send_with_pads(orch,
            target_method="validateOrderSignature",
            target_args=[list(order_hash), signed.to_abi_list()])
