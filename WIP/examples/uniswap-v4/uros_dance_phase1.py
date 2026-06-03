#!/usr/bin/env python3
"""Phase 1 of the PoolManager uros runtime port: prove the chunk-swap dance on a
single, Helper1-free method end-to-end on localnet.

Target = `setOperator`/`isOperator` (ERC6909), both in the `shell` chunk (no
Helper1 sidecar dependency, no pool/asset setup). The dance:

    [ UrosSetup.prepare()       ,  main.setOperator(prepare, operator, approved) ]
        swap chunk_shell into main (inner)   real body runs TOP-LEVEL on main

`setOperator` writes `isOperator[msg.sender][operator] = approved` and returns
true; reading it back via `isOperator(dispenser, operator)` proves (a) the swap
made the real body live, (b) state persisted, and (c) msg.sender was the real
top-level caller (the dispenser), not an inner-call forwarder. We deliberately
load ONLY clear + chunk_shell and map ONLY these two selectors — the init/swap/
modliq chunks carry TMPL_PoolManager__Helper1_APP_ID and are deferred to phase 2.

Run against a running algokit localnet:
    python WIP/examples/uniswap-v4/uros_dance_phase1.py
"""
# ruff: noqa: T201
from __future__ import annotations

import json
import math
from pathlib import Path

import algokit_utils as au

OUT = Path("/tmp/pm_sidecar/PoolManager")  # puya-sol --split-config output
WRITE_CHUNK = 2000  # <= ABI byte[] arg budget per write_box
MAX_REFS = 8
WRITE_BUDGET = 4096


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def main() -> None:
    manifest = json.loads((OUT / "deploy.uros.json").read_text())
    main_name = manifest["main_contract"]      # PoolManager
    setup_name = manifest["setup_contract"]     # UrosSetup
    methods = manifest["methods"]

    shell_program = (OUT / f"{main_name}.approval.bin").read_bytes()
    clear_program = (OUT / f"{main_name}.clear.bin").read_bytes()
    sch = json.loads((OUT / f"{main_name}.arc56.json").read_text())["state"]["schema"]

    # phase 1: only the Helper1-free shell chunk
    shell_chunk = (OUT / f"{main_name}__chunk_shell.approval.bin").read_bytes()
    selectors = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in methods}
    method_chunk = {m["name"]: m["chunk"] for m in methods}
    assert method_chunk["setOperator"] == "shell" and method_chunk["isOperator"] == "shell"

    # main must be created with enough extra pages to receive the largest chunk we load
    main_extra_pages = math.ceil(len(shell_chunk) / 2048) - 1

    algorand = au.AlgorandClient.default_localnet()
    dispenser = algorand.account.localnet_dispenser()
    algorand.set_default_signer(dispenser.signer)
    sender = dispenser.address
    operator = algorand.account.random().address  # just a key in the mapping; no funding needed

    # 1. deploy UrosSetup + fund for box MBR (clear + one ~7.5 KB chunk box)
    setup_factory = au.AppFactory(
        au.AppFactoryParams(
            algorand=algorand,
            app_spec=au.Arc56Contract.from_json((OUT / f"{setup_name}.arc56.json").read_text()),
            default_sender=sender,
        )
    )
    setup_client, _ = setup_factory.send.bare.create()
    setup_app_id = setup_client.app_id
    print(f"setup app id = {setup_app_id}")
    algorand.send.payment(
        au.PaymentParams(
            sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(15)
        )
    )

    # 2. create main from its shell, with extra pages so it can receive chunk_shell
    main_create = algorand.send.app_create(
        au.AppCreateParams(
            sender=sender,
            approval_program=shell_program,
            clear_state_program=clear_program,
            extra_program_pages=main_extra_pages,
            schema={
                "global_ints": sch["global"]["ints"],
                "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"],
                "local_byte_slices": sch["local"]["bytes"],
            },
        )
    )
    main_app_id = main_create.app_id
    print(f"main app id = {main_app_id} (extra pages={main_extra_pages})")
    # PoolManager's arc56 has `int24` method sigs (initialize/modifyLiquidity ticks) — Algorand
    # ABI has no signed ints, so algosdk rejects the whole spec on load. We only call the three
    # int24-free methods below, so filter the spec to those (a real client of int24 methods would
    # need puya-sol to emit a valid ABI type instead — flagged for the port).
    main_spec_dict = json.loads((OUT / f"{main_name}.arc56.json").read_text())
    KEEP = {"uros_set_setup", "setOperator", "isOperator"}
    main_spec_dict["methods"] = [m for m in main_spec_dict["methods"] if m["name"] in KEEP]
    main_client = au.AppClient(
        au.AppClientParams(
            app_id=main_app_id,
            algorand=algorand,
            app_spec=au.Arc56Contract.from_dict(main_spec_dict),
            default_sender=sender,
        )
    )
    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_app_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_app_id]))

    # main writes ERC6909 `isOperator` into a box (a Solidity mapping -> box), so its app account
    # needs MBR. (The RiskEngine example used only global state, so its main needed no funding.)
    algorand.send.payment(
        au.PaymentParams(
            sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_algo(1)
        )
    )

    # 3. create + load codeboxes: clear program + the shell chunk
    boxes: list[tuple[bytes, bytes]] = [(b"clear", clear_program), (b"shell", shell_chunk)]
    setup_client.send.call(
        au.AppClientMethodCallParams(
            method="create_codeboxes",
            args=[[(k, len(d)) for k, d in boxes]],
            box_references=[k for k, _ in boxes],
            static_fee=au.AlgoAmount.from_micro_algo(6000),
        ),
        send_params={"populate_app_call_resources": True},
    )
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(
                au.AppClientMethodCallParams(
                    method="write_box", args=[key, off, data[off : off + WRITE_CHUNK]],
                    box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000),
                )
            )
    print(f"loaded {len(boxes)} codeboxes (clear + shell chunk = {len(shell_chunk)} B)")

    # 4. map the two selectors -> shell chunk key
    for name in ("setOperator", "isOperator"):
        sel = selectors[name]
        setup_client.send.call(
            au.AppClientMethodCallParams(
                method="map_method", args=[sel, b"shell"],
                box_references=[b"m" + sel], static_fee=au.AlgoAmount.from_micro_algo(2000),
            )
        )
    print("mapped setOperator + isOperator -> shell")

    def prepare_call(method: str) -> au.AppCallMethodCallParams:
        sel = selectors[method]
        return setup_client.params.call(
            au.AppClientMethodCallParams(
                method="prepare", args=[],
                app_references=[main_app_id],                          # 1 app + 7 box = 8 (cap)
                box_references=[b"m" + sel, b"shell", b"clear", *empties(4)],
                static_fee=au.AlgoAmount.from_micro_algo(2000),
            )
        )

    # 5. THE DANCE — setOperator(operator, true), top-level, msg.sender = dispenser
    print("=== dance: [prepare, main.setOperator(prepare, operator, true)] ===")
    r_set = main_client.send.call(
        au.AppClientMethodCallParams(
            method="setOperator", args=[prepare_call("setOperator"), operator, True],
            box_references=empties(4),
        )
    )
    print(f"  setOperator abi_return = {r_set.abi_return}")
    assert r_set.abi_return is True, f"setOperator should return true, got {r_set.abi_return}"

    # 6. read it back: isOperator[dispenser][operator] must be true (proves msg.sender=dispenser)
    print("=== dance: [prepare, main.isOperator(prepare, dispenser, operator)] ===")
    r_get = main_client.send.call(
        au.AppClientMethodCallParams(
            method="isOperator", args=[prepare_call("isOperator"), sender, operator],
            box_references=empties(4),
        )
    )
    print(f"  isOperator(dispenser, operator) = {r_get.abi_return}")
    assert r_get.abi_return is True, "isOperator[dispenser][operator] should be true"

    # 7. directionality / not-always-true: isOperator[operator][dispenser] must be FALSE
    r_rev = main_client.send.call(
        au.AppClientMethodCallParams(
            method="isOperator", args=[prepare_call("isOperator"), operator, sender],
            box_references=empties(4),
        )
    )
    print(f"  isOperator(operator, dispenser) = {r_rev.abi_return}  (expect False)")
    assert r_rev.abi_return is False, "reverse direction should be false (proves real lookup)"

    print("\nPASS: chunk-swap dance works end-to-end — swap + guard + real top-level "
          "msg.sender + state persistence, on the puya-sol-compiled PoolManager.")


if __name__ == "__main__":
    main()
