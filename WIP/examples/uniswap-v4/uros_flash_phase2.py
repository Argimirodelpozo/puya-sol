#!/usr/bin/env python3
"""Phase 2 of the PoolManager uros runtime port: prove the AVM atomic-group
flash-accounting model (re-entrancy replaced by a group; deltas in scratch).

V4's `unlock` opens a lock, calls back into the integrator who RE-ENTERS the
PoolManager, then asserts all currency deltas are zero. Re-entrancy is forbidden
on the AVM, so we express the same guarantee as ONE atomic group of top-level
calls — no callback, no re-entry. Per-currency deltas live in SCRATCH (group-
scoped, ephemeral; slots 6/7 = debit/credit), each op records its own
contribution, and whichever op is LAST sums them across the group (via gload)
and requires net-zero.

We use mint/burn (ERC6909 claims) because they exercise the full delta path with
NO external token movement, int24, or Helper1 — pure single-currency flash:

  balanced:   [unlock, mint(id, A), burn(id, A)]   -> debit A == credit A -> OK
  unbalanced: [unlock, mint(id, A)]                -> debit A, credit 0   -> REVERT

Each op runs as a real top-level main.<method> call, preceded by its
UrosSetup.prepare (the chunk-swap dance), so the group is actually
[prepare, unlock, prepare, mint, prepare, burn].

Run against a running algokit localnet:
    python WIP/examples/uniswap-v4/uros_flash_phase2.py
"""
# ruff: noqa: T201
from __future__ import annotations

import json
import math
from pathlib import Path

import algokit_utils as au

OUT = Path("/tmp/pm_scratch")
WRITE_CHUNK = 2000
ID = 42        # arbitrary ERC6909 currency id
AMOUNT = 1000  # flash amount (uint64-range; minimal model is uint64-valued)


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def main() -> None:
    manifest = json.loads((OUT / "deploy.uros.json").read_text())
    main_name = manifest["main_contract"]
    setup_name = manifest["setup_contract"]
    methods = manifest["methods"]
    selectors = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in methods}

    shell_program = (OUT / f"{main_name}.approval.bin").read_bytes()
    clear_program = (OUT / f"{main_name}.clear.bin").read_bytes()
    shell_chunk = (OUT / f"{main_name}__chunk_shell.approval.bin").read_bytes()
    sch = json.loads((OUT / f"{main_name}.arc56.json").read_text())["state"]["schema"]
    main_extra_pages = math.ceil(len(shell_chunk) / 2048) - 1

    algorand = au.AlgorandClient.default_localnet()
    dispenser = algorand.account.localnet_dispenser()
    algorand.set_default_signer(dispenser.signer)
    sender = dispenser.address

    # --- deploy setup + main ---
    setup_factory = au.AppFactory(au.AppFactoryParams(
        algorand=algorand,
        app_spec=au.Arc56Contract.from_json((OUT / f"{setup_name}.arc56.json").read_text()),
        default_sender=sender,
    ))
    setup_client, _ = setup_factory.send.bare.create()
    setup_app_id = setup_client.app_id
    print(f"setup app id = {setup_app_id}")
    algorand.send.payment(au.PaymentParams(
        sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(15)))

    main_create = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell_program, clear_state_program=clear_program,
        extra_program_pages=main_extra_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]},
    ))
    main_app_id = main_create.app_id
    print(f"main app id = {main_app_id} (extra pages={main_extra_pages})")

    # arc56 filtered to int24-free methods we call
    main_spec_dict = json.loads((OUT / f"{main_name}.arc56.json").read_text())
    KEEP = {"uros_set_setup", "unlock", "mint", "burn", "balanceOf"}
    main_spec_dict["methods"] = [m for m in main_spec_dict["methods"] if m["name"] in KEEP]
    main_client = au.AppClient(au.AppClientParams(
        app_id=main_app_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_dict(main_spec_dict), default_sender=sender))

    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_app_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_app_id]))
    # main writes ERC6909 balances into boxes -> fund its account for MBR
    algorand.send.payment(au.PaymentParams(
        sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_algo(2)))

    # --- load clear + shell chunk into codeboxes, map unlock/mint/burn ---
    boxes = [(b"clear", clear_program), (b"shell", shell_chunk)]
    setup_client.send.call(
        au.AppClientMethodCallParams(method="create_codeboxes", args=[[(k, len(d)) for k, d in boxes]],
            box_references=[k for k, _ in boxes], static_fee=au.AlgoAmount.from_micro_algo(6000)),
        send_params={"populate_app_call_resources": True})
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(au.AppClientMethodCallParams(
                method="write_box", args=[key, off, data[off:off + WRITE_CHUNK]],
                box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    for name in ("unlock", "mint", "burn", "balanceOf"):
        sel = selectors[name]
        setup_client.send.call(au.AppClientMethodCallParams(
            method="map_method", args=[sel, b"shell"],
            box_references=[b"m" + sel], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print("deployed + loaded shell chunk + mapped unlock/mint/burn")

    def prepare_call(method: str) -> au.AppCallMethodCallParams:
        sel = selectors[method]
        return setup_client.params.call(au.AppClientMethodCallParams(
            method="prepare", args=[], app_references=[main_app_id],
            box_references=[b"m" + sel, b"shell", b"clear", *empties(4)],
            static_fee=au.AlgoAmount.from_micro_algo(3000)))

    def op(method: str, args: list):
        # main.<method>(prepare, *args): the ABI composer places `prepare`
        # immediately before this call -> [prepare, method] inside the group.
        return main_client.params.call(au.AppClientMethodCallParams(
            method=method, args=[prepare_call(method), *args], box_references=empties(4)))

    # ---- TEST 1: balanced [unlock, mint(A), burn(A)] -> succeeds ----
    print(f"\n=== TEST 1: balanced group [unlock, mint({AMOUNT}), burn({AMOUNT})] ===")
    g = algorand.new_group()
    g.add_app_call_method_call(op("unlock", [b""]))
    g.add_app_call_method_call(op("mint", [sender, ID, AMOUNT]))
    g.add_app_call_method_call(op("burn", [sender, ID, AMOUNT]))
    g.send()
    print("  OK: balanced group settled (net-zero debit==credit across the group)")

    # confirm mint/burn really executed: balanceOf(sender, ID) == 0 (minted then burned)
    bal = main_client.send.call(au.AppClientMethodCallParams(
        method="balanceOf", args=[prepare_call("balanceOf"), sender, ID], box_references=empties(4)))
    print(f"  balanceOf(sender, {ID}) = {bal.abi_return} (expect 0: minted {AMOUNT}, burned {AMOUNT})")
    assert bal.abi_return == 0, "mint/burn net balance should be 0"

    # ---- TEST 2: unbalanced [unlock, mint(A)] -> reverts (debit A != credit 0) ----
    print(f"\n=== TEST 2: unbalanced group [unlock, mint({AMOUNT})] -> must REVERT ===")
    try:
        g2 = algorand.new_group()
        g2.add_app_call_method_call(op("unlock", [b""]))
        g2.add_app_call_method_call(op("mint", [sender, ID, AMOUNT]))
        g2.send()
    except Exception as e:
        msg = " ".join(str(e).split())
        print(f"  OK rejected: unbalanced group reverted ({msg[:90]}...)")
    else:
        raise AssertionError("unbalanced group was NOT rejected — net-zero check failed!")

    # balance still 0 (TEST 2's mint was rolled back by the atomic revert)
    bal2 = main_client.send.call(au.AppClientMethodCallParams(
        method="balanceOf", args=[prepare_call("balanceOf"), sender, ID], box_references=empties(4)))
    assert bal2.abi_return == 0, "reverted mint should leave balance unchanged"
    print(f"  balanceOf still {bal2.abi_return} (the reverted mint was atomically undone)")

    print("\nPASS: AVM atomic-group flash accounting works — deltas in scratch, "
          "net-zero enforced at the last op, unbalanced groups revert atomically. "
          "No re-entrancy, no callback.")


if __name__ == "__main__":
    main()
