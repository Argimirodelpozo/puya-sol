"""Translation of v2 src/test/ERC1271Signature.t.sol.

Exercises validateOrderSignature with POLY_1271 sigType. The dispatch
goes orch → helper3 (`_verifyPoly1271Signature`) → helper1
(`SignatureCheckerLib.isValidSignatureNow`). Five revert tests don't
depend on the inner-call working; the happy path does — see
`_verifyPoly1271Signature` AVM-PORT-ADAPTATION below.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from conftest import AUTO_POPULATE, addr, app_id_to_address
from dev.orders import make_order, sign_order, hash_order_via_contract, Side, SignatureType
from dev.signing import bob, carla


def _call(client, method, args, sender=None, extra_fee=80_000, app_refs=None):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args,
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        app_references=app_refs or [],
    ), send_params=AUTO_POPULATE).abi_return


@pytest.fixture
def exchange(split_exchange):
    _, _, orch = split_exchange
    return orch


def _build_signed_order(exchange, *, maker_app_addr32: bytes, signer_eth20: bytes,
                        sig_type: SignatureType, sign_with):
    """Build + sign an order. `maker_app_addr32` is the contract wallet's
    32-byte AVM address. `signer_eth20` is the signer field (eth-style 20
    bytes, padded). `sign_with` is the EthSigner whose key produces the
    signature."""
    order = make_order(
        salt=1,
        maker=maker_app_addr32,
        signer=signer_eth20,
        token_id=12345,
        maker_amount=50_000_000,
        taker_amount=100_000_000,
        side=Side.BUY,
        signature_type=sig_type,
    )
    return sign_order(exchange, order, sign_with)


def test_validate_1271_signature(exchange, erc1271_mock_factory):
    """test_ERC1271Signature_validate1271Signature.

    Mock holds carla's eth address as inner signer. Order signed by carla,
    maker = mock app addr. Validates."""
    carla_signer = carla()
    wallet = erc1271_mock_factory(carla_signer.eth_address)
    wallet_addr32 = app_id_to_address(wallet.app_id)
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=wallet_addr32,
        signer_eth20=wallet_addr32,
        sig_type=SignatureType.POLY_1271,
        sign_with=carla_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()],
          app_refs=[wallet.app_id])


def test_validate_1271_signature_revert_incorrect_signer(exchange, erc1271_mock_factory):
    """Mock holds carla, but order is signed by bob. isValidSignature
    recovers bob ≠ carla → returns 0 → InvalidSignature revert."""
    carla_signer = carla()
    bob_signer = bob()
    wallet = erc1271_mock_factory(carla_signer.eth_address)
    wallet_addr32 = app_id_to_address(wallet.app_id)
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=wallet_addr32,
        signer_eth20=wallet_addr32,
        sig_type=SignatureType.POLY_1271,
        sign_with=bob_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    with pytest.raises(LogicError):
        _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()],
              app_refs=[wallet.app_id])


def test_validate_1271_signature_revert_sig_type(exchange, erc1271_mock_factory):
    """sigType = EOA but maker is a contract wallet. EOA path requires
    recovered_signer == maker; recovered = carla (20-byte EOA),
    maker = wallet (32-byte app addr). Mismatch → InvalidSignature."""
    carla_signer = carla()
    wallet = erc1271_mock_factory(carla_signer.eth_address)
    wallet_addr32 = app_id_to_address(wallet.app_id)
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=wallet_addr32,
        signer_eth20=wallet_addr32,
        sig_type=SignatureType.EOA,
        sign_with=carla_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    with pytest.raises(LogicError):
        _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()],
              app_refs=[wallet.app_id])


def test_validate_1271_signature_revert_non_contract(exchange):
    """Maker is an EOA, not a contract. `extcodesize(maker) > 0` check
    fails on the AVM port (it asks `app_params_get AppApprovalProgram` for
    the address; EOAs return zero) → InvalidSignature."""
    carla_signer = carla()
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=carla_signer.eth_address_padded32,
        signer_eth20=carla_signer.eth_address_padded32,
        sig_type=SignatureType.POLY_1271,
        sign_with=carla_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    with pytest.raises(LogicError):
        _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()])


def test_validate_1271_signature_revert_invalid_contract(exchange, usdc):
    """Maker is the USDC contract — has code but no `isValidSignature`.
    isValidSignatureNow returns false → InvalidSignature."""
    carla_signer = carla()
    usdc_addr32 = app_id_to_address(usdc.app_id)
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=usdc_addr32,
        signer_eth20=usdc_addr32,
        sig_type=SignatureType.POLY_1271,
        sign_with=carla_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    with pytest.raises(LogicError):
        _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()],
              app_refs=[usdc.app_id])


def test_validate_1271_signature_revert_invalid_signer_maker(exchange, erc1271_mock_factory):
    """signer = carla (EOA), maker = wallet (contract). The 1271 path
    short-circuits on `signer == maker` → InvalidSignature."""
    carla_signer = carla()
    wallet = erc1271_mock_factory(carla_signer.eth_address)
    wallet_addr32 = app_id_to_address(wallet.app_id)
    signed = _build_signed_order(
        exchange,
        maker_app_addr32=wallet_addr32,
        signer_eth20=carla_signer.eth_address_padded32,
        sig_type=SignatureType.POLY_1271,
        sign_with=carla_signer,
    )
    h = hash_order_via_contract(exchange, signed)
    with pytest.raises(LogicError):
        _call(exchange, "validateOrderSignature", [list(h), signed.to_abi_list()],
              app_refs=[wallet.app_id])
