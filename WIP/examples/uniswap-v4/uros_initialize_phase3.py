#!/usr/bin/env python3
"""Phase 3: prove a Helper1-dependent, int24-using PoolManager method runs e2e —
`initialize` (creates a pool, computes the initial tick via TickMath, no token
movement). This is the integration milestone for: the int24 ABI fix, the Helper1
sidecar wiring (TMPL substitution + inner-call), and the pool-state box.

initialize is in chunk_init, which inner-calls Helper1.getTickAtSqrtPrice via
ApplicationID = TMPL_PoolManager__Helper1_APP_ID. So we: deploy Helper1; replace
that template var in the chunk_init TEAL with Helper1's real app id; compile +
load it as the chunk; and pass Helper1 in the initialize call's app references so
the inner call can reach it.

int24 handling (NO compiler change): PoolManager's arc56 carries the honest signed
`int24` (Algorand ABI is uint-only, but puya-sol emits intN by design). Plain
algosdk rejects "int24"; the semantic-test framework ships an algosdk monkeypatch
(framework/_algosdk_patch.py) that teaches it intN — parse + two's-complement
encode/decode. This harness loads that patch at import (see top), so it calls
int24 methods with the int-based selector the deployed router uses, exactly like
the semantic suite. Same client-side approach for swap/modifyLiquidity later.

Requires: PoolManager compiled with --split-config (Helper1 sidecar).

    python WIP/examples/uniswap-v4/uros_initialize_phase3.py
"""
# ruff: noqa: T201
from __future__ import annotations

import base64
import importlib.util as _ilu
import json
import math
from pathlib import Path

import algokit_utils as au
import algosdk

# int{N} (signed) ABI support — NO compiler change. puya-sol intentionally emits a
# Solidity `intN` as the honest signed "intN" in the arc56 (Algorand ABI itself is
# uint-only); the semantic-test framework ships an algosdk monkeypatch that teaches
# algosdk to parse "intN" + two's-complement encode/decode. Loading it lets this
# harness call int24 methods (PoolKey.tickSpacing etc.) with the int-based selector
# the deployed router uses — exactly how the semantic suite handles signed ints.
_patch = Path(__file__).resolve().parents[3] / "tests/solidity-semantic-tests/framework/_algosdk_patch.py"
_spec = _ilu.spec_from_file_location("_algosdk_int_patch", _patch)
_spec.loader.exec_module(_ilu.module_from_spec(_spec))

OUT = Path("/tmp/pm_full")          # puya-sol --split-config output (PoolManager/ subdir)
PMDIR = OUT / "PoolManager"
HELPERDIR = OUT / "PoolManager__Helper1"  # SimpleSplitter sidecar (its own dir)
WRITE_CHUNK = 2000
ZERO_ADDR = algosdk.encoding.encode_address(bytes(32))  # address(0): native currency / no hooks


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def main() -> None:
    manifest = json.loads((PMDIR / "deploy.uros.json").read_text())
    selectors = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in manifest["methods"]}
    method_chunk = {m["name"]: m["chunk"] for m in manifest["methods"]}

    shell_program = (PMDIR / "PoolManager.approval.bin").read_bytes()
    clear_program = (PMDIR / "PoolManager.clear.bin").read_bytes()
    sch = json.loads((PMDIR / "PoolManager.arc56.json").read_text())["state"]["schema"]

    algorand = au.AlgorandClient.default_localnet()
    dispenser = algorand.account.localnet_dispenser()
    algorand.set_default_signer(dispenser.signer)
    sender = dispenser.address

    # --- deploy Helper1 (self-contained sidecar; the extracted tick-math) ---
    helper_factory = au.AppFactory(au.AppFactoryParams(
        algorand=algorand,
        app_spec=au.Arc56Contract.from_json((HELPERDIR / "PoolManager__Helper1.arc56.json").read_text()),
        default_sender=sender))
    helper_client, _ = helper_factory.send.bare.create()
    helper1_id = helper_client.app_id
    print(f"Helper1 app id = {helper1_id}")
    algorand.send.payment(au.PaymentParams(
        sender=sender, receiver=helper_client.app_address, amount=au.AlgoAmount.from_algo(1)))

    # --- the chunk we need for initialize, with Helper1's id substituted in ---
    def chunk_bytes(name: str) -> bytes:
        teal = (PMDIR / f"PoolManager__chunk_{name}.approval.teal").read_text()
        if "TMPL_PoolManager__Helper1_APP_ID" in teal:
            teal = teal.replace("TMPL_PoolManager__Helper1_APP_ID", str(helper1_id))
            res = algorand.client.algod.compile(teal)
            return base64.b64decode(res["result"])
        return (PMDIR / f"PoolManager__chunk_{name}.approval.bin").read_bytes()

    init_chunk = chunk_bytes("init")
    print(f"chunk_init (Helper1 id substituted) = {len(init_chunk)} B")

    # --- deploy setup + main ---
    setup_factory = au.AppFactory(au.AppFactoryParams(
        algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "UrosSetup.arc56.json").read_text()),
        default_sender=sender))
    setup_client, _ = setup_factory.send.bare.create()
    setup_app_id = setup_client.app_id
    algorand.send.payment(au.PaymentParams(
        sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(15)))

    main_extra_pages = math.ceil(len(init_chunk) / 2048) - 1
    main_create = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell_program, clear_state_program=clear_program,
        extra_program_pages=main_extra_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]}))
    main_app_id = main_create.app_id
    print(f"setup={setup_app_id} main={main_app_id} (extra pages={main_extra_pages})")

    # Full arc56 — the algosdk int{N} patch (loaded above) lets it parse the signed
    # int24 in PoolKey, so no method filtering needed; the selector stays int-based
    # and matches the deployed router.
    main_client = au.AppClient(au.AppClientParams(
        app_id=main_app_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "PoolManager.arc56.json").read_text()),
        default_sender=sender))

    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_app_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_app_id]))
    algorand.send.payment(au.PaymentParams(
        sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_algo(2)))

    # --- load codeboxes (clear + init chunk) + map initialize ---
    boxes = [(b"clear", clear_program), (b"init", init_chunk)]
    setup_client.send.call(
        au.AppClientMethodCallParams(method="create_codeboxes", args=[[(k, len(d)) for k, d in boxes]],
            box_references=[k for k, _ in boxes], static_fee=au.AlgoAmount.from_micro_algo(8000)),
        send_params={"populate_app_call_resources": True})
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(au.AppClientMethodCallParams(
                method="write_box", args=[key, off, data[off:off + WRITE_CHUNK]],
                box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    sel = selectors["initialize"]
    setup_client.send.call(au.AppClientMethodCallParams(
        method="map_method", args=[sel, method_chunk["initialize"].encode()],
        box_references=[b"m" + sel], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print("deployed + loaded init chunk + mapped initialize")

    # --- THE CALL: [prepare(init), initialize(poolKey, sqrtPriceX96)] ---
    # currency0 < currency1 (required). Use two low, distinct, ordered addresses.
    c0 = ZERO_ADDR  # address(0) sorts first (native sentinel; we never move tokens — no settle)
    # a non-zero currency1 (any address > 0). Use the dispenser's address.
    c1 = sender
    fee = 3000
    tick_spacing = 60
    sqrt_price_x96 = 79228162514264337593543950336  # 2**96 => price 1.0 => tick ~0
    pool_key = [c0, c1, fee, tick_spacing, ZERO_ADDR]  # hooks = address(0) => no hook calls

    # opcode-budget pool: TickMath.getTickAtSqrtPrice (Helper1 inner call) costs
    # ~1799 > the 700/app-call budget. Each extra app call in the group adds 700 to
    # the shared pool, so deploy a trivial opup app and pad the group with no-op
    # calls to it.
    opup_teal = "#pragma version 10\nint 1\nreturn\n"
    opup_compiled = base64.b64decode(algorand.client.algod.compile(opup_teal)["result"])
    opup_create = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=opup_compiled, clear_state_program=opup_compiled))
    opup_id = opup_create.app_id
    print(f"opup app id = {opup_id}")

    prep = setup_client.params.call(au.AppClientMethodCallParams(
        method="prepare", args=[], app_references=[main_app_id],
        box_references=[b"m" + sel, b"init", b"clear", *empties(4)],
        static_fee=au.AlgoAmount.from_micro_algo(3000)))

    print("=== calling initialize via the dance (Helper1 inner-call for the tick) ===")
    g = algorand.new_group()
    # 8 opup no-ops => +5600 pooled opcode budget for the inner tick-math call
    for i in range(8):
        g.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"opup{i}".encode(),
                                        on_complete=algosdk.transaction.OnComplete.NoOpOC,
                                        static_fee=au.AlgoAmount.from_micro_algo(1000)))
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="initialize", args=[prep, pool_key, sqrt_price_x96],
        app_references=[helper1_id],   # so chunk_init's inner call can reach Helper1
        box_references=empties(6),
        static_fee=au.AlgoAmount.from_micro_algo(6000))))   # outer + inner-call tree
    result = g.send()
    rets = [r.value for r in (result.returns or []) if r is not None and r.value is not None]
    print(f"  initialize -> tick = {rets[-1] if rets else '(no return decoded)'}")
    print("\nPASS: initialize ran e2e — int24 ABI + Helper1 sidecar inner-call + pool-state box "
          "all work through the chunk-swap dance.")


if __name__ == "__main__":
    main()
