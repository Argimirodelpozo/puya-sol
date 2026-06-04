#!/usr/bin/env python3
"""Phase 7: a V4 currency that is a REAL AERC20 (ASA-backed ERC20), with real token
movement (#53). Flash flow on the PoolManager shell chunk:

  [unlock, take(AERC20, user, X), axfer(user->pool, X), settle] -> net-zero

proving:
  * take(AERC20) moves a real ASA out of the pool — CurrencyLibrary.transfer (ported
    to a plain external call) -> AERC20.transfer -> asaTransfer clawback.
  * settle reads the AERC20 the user sent back (the grouped axfer at GroupIndex-2).
  * the conflated single-currency net-zero holds (take debit X == settle credit X).
  * the pool holds the ASA via optInAsset (AVM.asaOptIn).

The Currency for an AERC20 is the puya-sol external-call encoding: 0x{24 zeros}{app_id}.
Each op is a real top-level main.<method> call preceded by its UrosSetup.prepare.

    python WIP/examples/uniswap-v4/uros_aerc20_phase7.py
"""
# ruff: noqa: T201
from __future__ import annotations

import base64
import json
import math
from pathlib import Path

import algokit_utils as au
import algosdk
from algosdk.transaction import (
    ApplicationCreateTxn,
    AssetTransferTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
)

PMDIR = Path("/tmp/pm_full/PoolManager")
AERC20_OUT = Path("/home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol/WIP/examples/aerc20-demo/out")
WRITE_CHUNK = 2000
SEED = 100_000   # AERC20 the pool holds (lendable)
FLASH = 1000     # flash amount: take then repay


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def main() -> None:
    algorand = au.AlgorandClient.default_localnet()
    disp = algorand.account.localnet_dispenser()
    algorand.set_default_signer(disp.signer)
    sender = disp.address
    algod = algorand.client.algod

    # ── deploy a real AERC20 (MyToken) ──────────────────────────────────────
    ta = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.approval.teal").read_text())["result"])
    tc = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.clear.teal").read_text())["result"])
    tx = ApplicationCreateTxn(sender, algod.suggested_params(), OnComplete.NoOpOC, ta, tc,
                              StateSchema(16, 16), StateSchema(0, 0))
    tok_id = wait_for_confirmation(algod, algod.send_transaction(tx.sign(disp.private_key)), 4)["application-index"]
    tok_addr = algosdk.logic.get_application_address(tok_id)
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=tok_addr, amount=au.AlgoAmount.from_algo(1)))
    token = au.AppClient(au.AppClientParams(app_id=tok_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((AERC20_OUT / "MyToken.arc56.json").read_text()), default_sender=sender))
    token.send.call(au.AppClientMethodCallParams(method="__postInit", args=[], extra_fee=au.AlgoAmount.from_micro_algo(2000)))
    asa = int(token.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return)
    # the V4 Currency for this AERC20 (puya-sol external-call address convention)
    currency = algosdk.encoding.encode_address(bytes(24) + tok_id.to_bytes(8, "big"))
    print(f"AERC20 token app={tok_id} asa={asa} currency={currency[:12]}..")

    # ── deploy V4 (setup + main + opup), load shell chunk, map methods ───────
    manifest = json.loads((PMDIR / "deploy.uros.json").read_text())
    sel = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in manifest["methods"]}
    shell_program = (PMDIR / "PoolManager.approval.bin").read_bytes()
    clear_program = (PMDIR / "PoolManager.clear.bin").read_bytes()
    shell_chunk = (PMDIR / "PoolManager__chunk_shell.approval.bin").read_bytes()
    sch = json.loads((PMDIR / "PoolManager.arc56.json").read_text())["state"]["schema"]
    max_pages = math.ceil(len(shell_chunk) / 2048) - 1

    opup_c = base64.b64decode(algod.compile("#pragma version 10\nint 1\nreturn\n")["result"])
    opup_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=opup_c, clear_state_program=opup_c)).app_id

    sf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "UrosSetup.arc56.json").read_text()), default_sender=sender))
    setup_client, _ = sf.send.bare.create()
    setup_id = setup_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(20)))

    main_id = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell_program, clear_state_program=clear_program, extra_program_pages=max_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]})).app_id
    main_addr = algosdk.logic.get_application_address(main_id)
    spec = json.loads((PMDIR / "PoolManager.arc56.json").read_text())
    spec["methods"] = [m for m in spec["methods"] if m["name"] in {"uros_set_setup", "unlock", "take", "settleCurrency", "optInAsset"}]
    main_client = au.AppClient(au.AppClientParams(app_id=main_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_dict(spec), default_sender=sender))
    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_id]))
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(2)))

    boxes = [(b"clear", clear_program), (b"shell", shell_chunk)]
    setup_client.send.call(au.AppClientMethodCallParams(method="create_codeboxes", args=[[(k, len(d)) for k, d in boxes]],
        box_references=[k for k, _ in boxes], static_fee=au.AlgoAmount.from_micro_algo(8000)), send_params={"populate_app_call_resources": True})
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(au.AppClientMethodCallParams(method="write_box", args=[key, off, data[off:off + WRITE_CHUNK]],
                box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    for name in ("unlock", "take", "settleCurrency", "optInAsset"):
        setup_client.send.call(au.AppClientMethodCallParams(method="map_method", args=[sel[name], b"shell"],
            box_references=[b"m" + sel[name]], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print(f"deployed V4: setup={setup_id} main={main_id} opup={opup_id}")

    def prep(name: str):
        return setup_client.params.call(au.AppClientMethodCallParams(method="prepare", args=[],
            app_references=[main_id], box_references=[b"m" + sel[name], b"shell", b"clear", *empties(4)],
            static_fee=au.AlgoAmount.from_micro_algo(3000)))

    def add_opup(g, n):
        for k in range(n):
            g.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"o{k}".encode(),
                on_complete=algosdk.transaction.OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(2000)))

    # ── opt the pool into the ASA, seed it with AERC20, opt the user in ──────
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(1)))  # MBR for the ASA
    main_client.send.call(au.AppClientMethodCallParams(method="optInAsset", args=[prep("optInAsset"), asa],
        box_references=empties(4), asset_references=[asa], static_fee=au.AlgoAmount.from_micro_algo(5000)))

    # ── SECURITY: a non-creator must NOT be able to optInAsset. Unguarded, anyone could
    #    force the pool into arbitrary ASAs, locking 0.1 ALGO MBR each (a balance-drain /
    #    DoS). The guard is `msg.sender == Global.creatorAddress()` (the same check the
    #    uros infra uses). Prove it on-chain: a funded non-creator's optInAsset reverts.
    attacker = algorand.account.random()
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=attacker.address, amount=au.AlgoAmount.from_algo(1)))
    atk_client = au.AppClient(au.AppClientParams(app_id=main_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_dict(spec), default_sender=attacker.address))
    rejected = False
    try:
        atk_client.send.call(au.AppClientMethodCallParams(method="optInAsset", args=[prep("optInAsset"), asa],
            box_references=empties(4), asset_references=[asa], static_fee=au.AlgoAmount.from_micro_algo(5000)))
    except Exception as e:  # noqa: BLE001
        rejected = any(s in str(e) for s in ("assert failed", "logic eval", "creator"))
    assert rejected, "SECURITY: non-creator optInAsset MUST be rejected (DoS/MBR-drain guard)"
    print("✅ non-creator optInAsset correctly REJECTED (only-creator guard holds)")

    token.send.call(au.AppClientMethodCallParams(method="mint", args=[main_addr, SEED],
        extra_fee=au.AlgoAmount.from_micro_algo(2000)), send_params={"populate_app_call_resources": True})  # seed the pool
    # user (sender) opts into the ASA
    algod.send_transaction(AssetTransferTxn(sender, algod.suggested_params(), sender, 0, asa).sign(disp.private_key))
    pool0 = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[main_addr])).abi_return)
    user0 = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[sender])).abi_return)
    print(f"seeded: pool holds {pool0} AERC20, user holds {user0}")

    # ── FLASH: [unlock, take(AERC20,user,X), axfer(user->pool,X), settle] ────
    print(f"\n=== FLASH take({FLASH}) + settle({FLASH}) AERC20, net-zero ===")
    g = algorand.new_group()
    add_opup(g, 4)
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="unlock", args=[prep("unlock"), b""], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(2000))))
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="take", args=[prep("take"), currency, sender, FLASH], app_references=[tok_id],
        asset_references=[asa], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=asa, amount=FLASH))  # repay
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="settleCurrency", args=[prep("settleCurrency"), currency], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(3000))))
    g.send({"populate_app_call_resources": True})
    pool1 = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[main_addr])).abi_return)
    user1 = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[sender])).abi_return)
    print(f"after flash: pool holds {pool1} AERC20, user holds {user1}")
    assert pool1 == pool0 and user1 == user0, "flash must net to zero (pool + user balances unchanged)"
    print("\n✅ PASS: V4 flash with a REAL AERC20 currency — take moved the ASA out "
          "(external call -> asaTransfer clawback), settle read the repaid axfer, "
          "atomic-group net-zero held. Real token movement through the PoolManager.")


if __name__ == "__main__":
    main()
