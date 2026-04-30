"""Translation of v2 src/test/MatchOrdersCtfCollateralAdapter.t.sol — 8 tests.

Exercises matchOrders settlement when:
  - collateral slot      = CollateralToken (pUSD)
  - outcomeTokenFactory  = CtfCollateralAdapter
  - ctfCollateral        = USDCe

AVM-port adaptation: maker eth-style identities (bob/carla) hold ECDSA
secp256k1 keys with no algorand presence, so they can't sign
`pUSD.approve(h1, _)` directly. We bridge via:
  1. `MakerWallet1271`: a contract that stores the eth address as
     `owner`, validates EIP-712 sigs via ecrecover, and exposes
     ERC-1271 `isValidSignature`. Order's `maker`/`signer` fields are
     the wallet's puya-sol-conv address (24 zeros + itob(app_id)) so
     matchOrders' POLY_1271 path's `extract_uint64(maker, 24) →
     app_params_get → inner-tx isValidSignature` chain dispatches.
  2. `CollateralToken.avmPortForceApprove`: admin-only test shim that
     writes the (owner → spender) allowance slot directly. Needed
     because the wallet's normal `CT.approve(h1, _)` would set the
     allowance under the wallet's algod-derived address (the inner-tx
     sender), but the matchOrders' h1.transferFrom looks up under
     `order.maker = wallet_psol` — different address space.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.encoding import decode_address
from hashlib import sha256

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.deals import (
    deal_usdc as deal_usdce, set_allowance as set_usdce_allowance,
    deal_outcome, set_approval, prepare_condition,
)
from dev.deploy import deploy_app
from dev.invoke import call
from dev.match_dispatch import dance_match_orders
from dev.orders import make_order, sign_order, Side, SignatureType
from dev.signing import bob, carla


CONDITION_ID = b"\xc0" * 32


def _deploy_maker_wallet(localnet, admin, *, owner_eth_addr32: bytes):
    """Deploy a MakerWallet1271 with `owner_eth_addr32` as the eth-key
    owner and `admin.address` as the admin (for approveERC20 hook)."""
    from conftest import OUT_DIR
    base = OUT_DIR / "test" / "dev" / "mocks" / "MakerWallet1271"
    return deploy_app(
        localnet, admin, base, "MakerWallet1271",
        create_args=[bytes(owner_eth_addr32),
                     bytes(decode_address(admin.address))],
    )


def _canonical_yes_no_ids(orch, h1, ctf_collateral_app_id):
    """Position ids derived from `ctf_collateral_app_id` (= USDCe in the
    adapter variant) — that's what adapter._getPositionIds passes.
    Uses the puya-sol-conv form (24 zeros + itob(app_id)) because
    helper3._validateTokenIds reads `ctfCollateral` from app_global_get
    which was populated with the puya-sol-conv form during __postInit."""
    coll_addr32 = bytes(app_id_to_address(ctf_collateral_app_id))

    def _pid(index_set):
        coll_id = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getCollectionId",
            args=[list(b"\x00" * 32), list(CONDITION_ID), index_set],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        coll_id_bytes = bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id
        pid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getPositionId",
            args=[bytes(coll_addr32), list(coll_id_bytes)],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        return int(pid)

    return _pid(1), _pid(2)


def _fund_wallet_pusd(
    *, wallet_psol, ct, usdce, onramp, vault, h1_app_id, admin, amount,
):
    """admin mints USDCe to itself, wraps via Onramp into pUSD credited
    to `wallet_psol` (puya-sol-conv form), then writes the allowance
    `wallet_psol → h1_algod` via CT.avmPortForceApprove."""
    admin32 = decode_address(admin.address)
    deal_usdce(usdce, admin32, amount)
    set_usdce_allowance(usdce, admin32,
                        algod_addr_bytes_for_app(onramp.app_id), amount)
    call(onramp, "wrap",
         [app_id_to_address(usdce.app_id), wallet_psol, amount],
         sender=admin)
    set_usdce_allowance(usdce, decode_address(vault.address),
                        algod_addr_bytes_for_app(ct.app_id), amount)
    call(ct, "avmPortForceApprove",
         [wallet_psol, algod_addr_bytes_for_app(h1_app_id), amount],
         sender=admin)


def _fund_wallet_outcome(ctf, wallet_psol, h1_addr, token_id, amount):
    """Force-mint `amount` of CTF outcome `token_id` to `wallet_psol` and
    flip setApprovalForAll(wallet_psol → h1_algod) on. The exchange's
    h1.transferFromERC1155 reads under `(wallet_psol, h1_algod)`."""
    deal_outcome(ctf, wallet_psol, token_id, amount)
    set_approval(ctf, wallet_psol, h1_addr, True)


def _wallets_and_ids(localnet, admin, orch, h1, usdce):
    """Common setup: deploy bob/carla MakerWallet1271 wrappers and
    compute YES/NO position ids the orch expects."""
    bob_signer, carla_signer = bob(), carla()
    bob_wallet = _deploy_maker_wallet(
        localnet, admin, owner_eth_addr32=bob_signer.eth_address_padded32)
    carla_wallet = _deploy_maker_wallet(
        localnet, admin, owner_eth_addr32=carla_signer.eth_address_padded32)
    bob_psol = bytes(app_id_to_address(bob_wallet.app_id))
    carla_psol = bytes(app_id_to_address(carla_wallet.app_id))
    yes_id, no_id = _canonical_yes_no_ids(orch, h1, usdce.app_id)
    return (bob_signer, carla_signer, bob_wallet, carla_wallet,
            bob_psol, carla_psol, yes_id, no_id)


# ── happy paths ────────────────────────────────────────────────────────


def test_MatchOrdersCtfCollateralAdapter_Mint(
    split_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Both BUYs of complementary tokens. Exchange takes both makers'
    pUSD via h1 (each maker is a 1271 wallet), calls adapter.splitPosition
    (unwraps pUSD → USDCe → CTF.splitPosition mints YES+NO to orch),
    then distributes YES → bob's wallet, NO → carla's wallet."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    _fund_wallet_pusd(
        wallet_psol=carla_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    # adapter.split unwraps the COMBINED 100M pUSD via CT.unwrap, which
    # pulls 100M USDCe from VAULT via transferFrom. The per-wallet fund
    # calls each set vault→ct allowance to 50M (overwriting); bump for
    # the combined unwrap.
    set_usdce_allowance(
        usdce, decode_address(vault.address),
        algod_addr_bytes_for_app(ct.app_id), 1_000_000_000,
    )

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(orch_addr) + adapter_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(orch_addr)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + bytes(decode_address(vault.address))),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(adapter_addr)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(
            app_id=usdce.app_id,
            name=b"a_" + sha256(bytes(adapter_addr) + bytes(algod_addr_bytes_for_app(ctf.app_id))).digest()),
        au.BoxReference(
            app_id=usdce.app_id,
            name=b"a_" + sha256(bytes(ct_addr) + bytes(adapter_addr)).digest()),
        au.BoxReference(app_id=ctf.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(adapter_addr) + yes_bytes).digest()),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(adapter_addr) + no_bytes).digest()),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(orch_addr) + yes_bytes).digest()),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(orch_addr) + no_bytes).digest()),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
        au.BoxReference(
            app_id=ctf.app_id,
            name=b"b_" + sha256(bytes(carla_psol) + no_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[ct.app_id, ctf.app_id, usdce.app_id,
                        adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 0


def test_MatchOrdersCtfCollateralAdapter_Mint_Fees(
    split_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Mint with fees: bob spends 52.5M pUSD (50M + 2.5M taker fee),
    carla spends 50.1M pUSD (50M + 0.1M maker fee). FeeReceiver pockets
    2.6M pUSD."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    taker_fee = 2_500_000
    maker_fee = 100_000
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + taker_fee,
    )
    _fund_wallet_pusd(
        wallet_psol=carla_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + maker_fee,
    )
    set_usdce_allowance(
        usdce, decode_address(vault.address),
        algod_addr_bytes_for_app(ct.app_id), 1_000_000_000,
    )

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(orch_addr) + adapter_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(orch_addr)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(fee_receiver)),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + bytes(decode_address(vault.address))),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(adapter_addr)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(
            app_id=usdce.app_id,
            name=b"a_" + sha256(bytes(adapter_addr) + bytes(algod_addr_bytes_for_app(ctf.app_id))).digest()),
        au.BoxReference(
            app_id=usdce.app_id,
            name=b"a_" + sha256(bytes(ct_addr) + bytes(adapter_addr)).digest()),
        au.BoxReference(app_id=ctf.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_psol) + no_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        extra_app_refs=[ct.app_id, ctf.app_id, usdce.app_id,
                        adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 0
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


def test_MatchOrdersCtfCollateralAdapter_Complementary(
    split_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Bob BUYs 100 YES at 50c (offers 50M pUSD), Carla SELLs 100M YES at
    50c (offers 100M YES for 50M pUSD). Direct P2P transfer — no adapter
    call."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, yes_id, 100_000_000)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        # CT (pUSD): bob's allowance to h1, balance from bob → carla
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
        # CTF: carla's approval to h1, balance carla → bob
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_psol) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[ct.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    # Bob: spent 50M pUSD, got 100M YES.
    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 100_000_000
    # Carla: spent 100M YES, got 50M pUSD.
    assert call(ctf, "balanceOf", [carla_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000


def test_MatchOrdersCtfCollateralAdapter_Complementary_Fees(
    split_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Complementary settlement with maker + taker fees. Bob BUYs 100M YES
    at 50c + 2.5M taker fee (52.5M total). Carla SELLs 100M YES, gets
    49.9M pUSD (50M - 0.1M maker fee). FeeReceiver pockets 2.6M pUSD."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    taker_fee = 2_500_000
    maker_fee = 100_000
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + taker_fee,
    )
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, yes_id, 100_000_000)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(fee_receiver)),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_psol) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        extra_app_refs=[ct.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 100_000_000
    assert call(ctf, "balanceOf", [carla_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000 - maker_fee
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


def test_MatchOrdersCtfCollateralAdapter_Merge(
    split_adapter_with_delegate, admin, localnet, vault,
):
    """Both SELLs of complementary tokens (YES + NO). Adapter merges
    YES+NO via CTF.mergePositions → 100M USDCe → CT.wrap → 100M pUSD,
    distributed 50M each to bob/carla."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    # CTFMock.mergePositions hands USDCe back to msg.sender (the adapter)
    # via an inner USDCe.transfer. Pre-fund CTFMock's USDCe pool — without
    # a prior splitPosition there's nothing for the merge to refund.
    ctf_algod = algod_addr_bytes_for_app(ctf.app_id)
    deal_usdce(usdce, ctf_algod, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)

    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    # adapter.mergePositions does CTF.safeBatchTransferFrom(orch, adapter,
    # ...) — needs orch's setApprovalForAll(adapter, true). The Assets
    # ctor's approval is keyed under `(orch_algod, adapter_psol)`, not
    # `(orch_algod, adapter_algod)` — wrong slot for what CTFMock checks.
    set_approval(ctf, orch_addr, adapter_addr, True)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        # CTF: maker→orch transfers + orch→adapter batch + burns
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(orch_addr) + bytes(adapter_addr)).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_psol) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id, name=b"p_" + CONDITION_ID),
        # USDCe: ctf pool → adapter → CT, then CT → vault on wrap
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ctf_algod)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(adapter_addr)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + bytes(decode_address(vault.address))),
        # CT: pUSD minted to orch, distributed to wallets
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(orch_addr)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=100_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[ct.app_id, ctf.app_id, usdce.app_id,
                        adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [bob_psol]) == 50_000_000
    assert call(ctf, "balanceOf", [carla_psol, no_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000


def test_MatchOrdersCtfCollateralAdapter_Merge_Fees(
    split_adapter_with_delegate, admin, localnet, vault,
):
    """Merge with fees: bob nets 49M pUSD (50M - 1M taker fee), carla
    nets 49.5M (50M - 0.5M maker fee), feeReceiver pockets 1.5M pUSD."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    ctf_algod = algod_addr_bytes_for_app(ctf.app_id)
    deal_usdce(usdce, ctf_algod, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)

    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    set_approval(ctf, orch_addr, adapter_addr, True)

    taker_fee = 1_000_000
    maker_fee = 500_000
    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(bob_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_psol) + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(orch_addr) + bytes(adapter_addr)).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_psol) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_psol) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(orch_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(adapter_addr) + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ctf_algod)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(adapter_addr)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + bytes(ct_addr)),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + bytes(decode_address(vault.address))),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(orch_addr)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(bob_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(carla_psol)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bytes(fee_receiver)),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=100_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        extra_app_refs=[ct.app_id, ctf.app_id, usdce.app_id,
                        adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [bob_psol]) == 50_000_000 - taker_fee
    assert call(ctf, "balanceOf", [carla_psol, no_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000 - maker_fee
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


# ── revert paths ───────────────────────────────────────────────────────


def test_MatchOrdersCtfCollateralAdapter_Merge_Reverts_WhenAdapterNotApproved(
    split_adapter_with_delegate, admin, localnet, vault,
):
    """Without orch's setApprovalForAll(adapter, true) on CTF, the
    adapter's batch transfer of YES+NO from orch to adapter reverts."""
    h1, _, orch, ct, ctf, usdce, adapter, chunk = split_adapter_with_delegate

    ctf_algod = algod_addr_bytes_for_app(ctf.app_id)
    deal_usdce(usdce, ctf_algod, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, orch, h1, usdce)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    # NB: deliberately skip `set_approval(ctf, orch, adapter, True)`.

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=100_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[ct.app_id, ctf.app_id, usdce.app_id,
                            adapter.app_id, h1.app_id],
            extra_box_refs=[
                au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
                au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
            ],
            budget_pad=15,
            per_pad_boxes=1,
        )


XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE = (
    "WhenAdapterUsdceMismatch needs a fully parallel exchange (h1/h2/orch/"
    "chunk) deployed pointed at a bad adapter whose USDCE differs from "
    "orch.ctfCollateral. The fixture machinery only builds one exchange "
    "per test; building a second inline is heavy enough that it warrants "
    "a dedicated fixture rather than test-local code. Tracked separately."
)


@pytest.mark.xfail(reason=XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint_Reverts_WhenAdapterUsdceMismatch():
    raise NotImplementedError(XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE)
