#!/usr/bin/env python3
"""Runtime pytest suite for the uros PoolManager (the real monolithic V4 contract,
chunked via the uros splitter + driven through the prepare dance).

A module-scoped fixture deploys the PoolManager once (setup/main/Helper1/chunks +
boosters + two real AERC20 currencies). Each test then initializes its OWN pool —
keyed by a distinct fee so pools don't collide — seeds it, and runs one scenario,
asserting per-currency balance deltas. Covers the core lifecycle:
  init, add-liquidity, swap zeroForOne, swap oneForZero, REMOVE-liquidity, and the
  cross-currency-cheat security check (per-currency net-zero, #44).

Factored from uros_swap_phase6 / uros_aerc20_phase7 / uros_bidir_phase8.

    pytest WIP/examples/uniswap-v4/test_uros_runtime.py -q
"""
import base64
import importlib.util as _ilu
import itertools
import json
import math
from pathlib import Path
from types import SimpleNamespace

import algokit_utils as au
import algosdk
import pytest
from algosdk.transaction import (
    ApplicationCreateTxn,
    AssetTransferTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
)
from algosdk.v2client import models as _m

_patch = Path(__file__).resolve().parents[3] / "tests/solidity-semantic-tests/framework/_algosdk_patch.py"
_spec = _ilu.spec_from_file_location("_algosdk_int_patch", _patch)
_spec.loader.exec_module(_ilu.module_from_spec(_spec))

PMDIR = Path("/tmp/pm_full/PoolManager")
HELPERDIR = Path("/tmp/pm_full/PoolManager__Helper1")
AERC20_OUT = Path(__file__).resolve().parent.parent / "aerc20-demo/out"
WRITE_CHUNK = 2000
ZERO_ADDR = algosdk.encoding.encode_address(bytes(32))
INIT_SQRT = int(1.0001 ** 45 * (2 ** 96))   # ~tick 90, inside [60,120]
LIMIT_DOWN = int(1.0001 ** 31 * (2 ** 96))  # ~tick 62
LIMIT_UP = int(1.0001 ** 59 * (2 ** 96))    # ~tick 118
SEED_LIQ = 5_000_000


def _empties(n):
    return [au.BoxReference(0, b"") for _ in range(n)]


def _cid(currency):
    return int.from_bytes(algosdk.encoding.decode_address(currency)[24:], "big")


def _deploy_aerc20(algorand, sender, disp):
    algod = algorand.client.algod
    ta = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.approval.teal").read_text())["result"])
    tc = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.clear.teal").read_text())["result"])
    tx = ApplicationCreateTxn(sender, algod.suggested_params(), OnComplete.NoOpOC, ta, tc, StateSchema(16, 16), StateSchema(0, 0))
    app_id = wait_for_confirmation(algod, algod.send_transaction(tx.sign(disp.private_key)), 4)["application-index"]
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=algosdk.logic.get_application_address(app_id), amount=au.AlgoAmount.from_algo(1)))
    tok = au.AppClient(au.AppClientParams(app_id=app_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((AERC20_OUT / "MyToken.arc56.json").read_text()), default_sender=sender))
    tok.send.call(au.AppClientMethodCallParams(method="__postInit", args=[], extra_fee=au.AlgoAmount.from_micro_algo(2000)))
    asa = int(tok.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return)
    currency = algosdk.encoding.encode_address(bytes(24) + app_id.to_bytes(8, "big"))
    return SimpleNamespace(app=app_id, asa=asa, currency=currency, tok=tok)


@pytest.fixture(scope="module")
def H():
    """Deploy the uros PoolManager once + two AERC20 currencies; return a harness handle."""
    algorand = au.AlgorandClient.default_localnet()
    disp = algorand.account.localnet_dispenser()
    algorand.set_default_signer(disp.signer)
    sender = disp.address
    algod = algorand.client.algod

    a, b = _deploy_aerc20(algorand, sender, disp), _deploy_aerc20(algorand, sender, disp)
    t0, t1 = sorted([a, b], key=lambda t: t.currency)

    manifest = json.loads((PMDIR / "deploy.uros.json").read_text())
    sel = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in manifest["methods"]}
    mchunk = {m["name"]: m["chunk"] for m in manifest["methods"]}
    shell = (PMDIR / "PoolManager.approval.bin").read_bytes()
    clear = (PMDIR / "PoolManager.clear.bin").read_bytes()
    sch = json.loads((PMDIR / "PoolManager.arc56.json").read_text())["state"]["schema"]

    hf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((HELPERDIR / "PoolManager__Helper1.arc56.json").read_text()), default_sender=sender))
    helper_client, _ = hf.send.bare.create()
    helper1_id = helper_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=helper_client.app_address, amount=au.AlgoAmount.from_algo(1)))

    def chunk_bytes(name):
        teal = (PMDIR / f"PoolManager__chunk_{name}.approval.teal").read_text()
        if "TMPL_PoolManager__Helper1_APP_ID" in teal:
            teal = teal.replace("TMPL_PoolManager__Helper1_APP_ID", str(helper1_id))
            return base64.b64decode(algod.compile(teal)["result"])
        return (PMDIR / f"PoolManager__chunk_{name}.approval.bin").read_bytes()

    chunks = {n: chunk_bytes(n) for n in ("init", "modliq", "swap", "shell", "donate")}
    max_pages = max(math.ceil(len(p) / 2048) for p in chunks.values()) - 1

    opup_c = base64.b64decode(algod.compile("#pragma version 10\nint 1\nreturn\n")["result"])
    opup_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=opup_c, clear_state_program=opup_c)).app_id
    boost_src = ("#pragma version 10\ntxn ApplicationID\nbz ok\nint 0\nloop:\ndup\nint 40\n<\nbz ok\n"
                 "itxn_begin\nint 6\nitxn_field TypeEnum\n"
                 f"int {opup_id}\nitxn_field ApplicationID\nint 0\nitxn_field Fee\nitxn_submit\nint 1\n+\nb loop\nok:\nint 1\nreturn\n")
    boost_c = base64.b64decode(algod.compile(boost_src)["result"])
    boost_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=boost_c, clear_state_program=boost_c)).app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=algosdk.logic.get_application_address(boost_id), amount=au.AlgoAmount.from_algo(1)))

    sf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "UrosSetup.arc56.json").read_text()), default_sender=sender))
    setup_client, _ = sf.send.bare.create()
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(25)))

    main_id = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell, clear_state_program=clear, extra_program_pages=max_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]})).app_id
    main_addr = algosdk.logic.get_application_address(main_id)
    main_client = au.AppClient(au.AppClientParams(app_id=main_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "PoolManager.arc56.json").read_text()), default_sender=sender))
    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_client.app_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_id]))
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(5)))

    boxes = [(b"clear", clear)] + [(n.encode(), chunks[n]) for n in chunks]
    i = 0
    while i < len(boxes):
        batch, total = [], 0
        while i < len(boxes) and len(batch) < 6 and (not batch or total + len(boxes[i][1]) < 4096 * 3):
            batch.append(boxes[i]); total += len(boxes[i][1]); i += 1
        setup_client.send.call(au.AppClientMethodCallParams(method="create_codeboxes", args=[[(k, len(d)) for k, d in batch]],
            box_references=[k for k, _ in batch], static_fee=au.AlgoAmount.from_micro_algo(8000)), send_params={"populate_app_call_resources": True})
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(au.AppClientMethodCallParams(method="write_box", args=[key, off, data[off:off + WRITE_CHUNK]],
                box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    for name in ("initialize", "unlock", "modifyLiquidity", "swap", "settleCurrency", "take", "optInAsset", "donate", "mint", "burn"):
        s = sel[name]
        setup_client.send.call(au.AppClientMethodCallParams(method="map_method", args=[s, mchunk[name].encode()],
            box_references=[b"m" + s], static_fee=au.AlgoAmount.from_micro_algo(2000)))

    # opt the pool into both ASAs + mint both to the user
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(2)))
    h = SimpleNamespace(algorand=algorand, sender=sender, algod=algod, main_client=main_client, main_addr=main_addr,
                        setup_client=setup_client, helper1_id=helper1_id, boost_id=boost_id, sel=sel, mchunk=mchunk,
                        t0=t0, t1=t1, c0=t0.currency, c1=t1.currency, ctr=itertools.count(1))
    for t in (t0, t1):
        _opt_in_pool(h, t.asa)
        algod.send_transaction(AssetTransferTxn(sender, algod.suggested_params(), sender, 0, t.asa).sign(disp.private_key))
        t.tok.send.call(au.AppClientMethodCallParams(method="mint", args=[sender, 800_000],
            extra_fee=au.AlgoAmount.from_micro_algo(2000)), send_params={"populate_app_call_resources": True})
    # ONE shared pool for the suite (file order = lifecycle; remove-liquidity runs last).
    # Per-test pools are also viable — box-refs POOL across the group, so dummy box-ref txns /
    # populate_app_call_resources cover >8 boxes — but a single pool keeps the suite simple.
    h.pool = _key(h, 3000)
    _init(h, h.pool)
    h.owe0, h.owe1 = _seed(h, h.pool)
    return h


# ── dance helpers (module-level, take the harness H) ──

def _nxt(H):
    return f"n{next(H.ctr)}".encode()


def _prep(H, name):
    s = H.sel[name]
    return H.setup_client.params.call(au.AppClientMethodCallParams(method="prepare", args=[],
        app_references=[H.main_client.app_id], box_references=[b"m" + s, H.mchunk[name].encode(), b"clear", *_empties(4)],
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(3000)))


def _boost(H, g, n=2):
    for k in range(n):
        g.add_app_call(au.AppCallParams(sender=H.sender, app_id=H.boost_id, note=f"b{k}".encode() + _nxt(H),
            on_complete=OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(45000)))


def _opt_in_pool(H, asa):
    H.main_client.send.call(au.AppClientMethodCallParams(method="optInAsset", args=[_prep(H, "optInAsset"), asa],
        box_references=_empties(4), asset_references=[asa], static_fee=au.AlgoAmount.from_micro_algo(5000)))


def _boxes(H):
    return [au.BoxReference(H.main_client.app_id, base64.b64decode(b["name"]))
            for b in H.algod.application_boxes(H.main_client.app_id).get("boxes", [])]


def _unlock(H, g):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="unlock", args=[_prep(H, "unlock"), b""], box_references=_empties(4), note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(2000))))


def _modliq(H, g, key, tl, tu, liq):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="modifyLiquidity", args=[_prep(H, "modifyLiquidity"), key, [tl, tu, liq, bytes(32)], b""],
        app_references=[H.helper1_id], box_references=[*_boxes(H), *_empties(6 - len(_boxes(H)))],
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(8000))))


def _swap(H, g, key, zfo, amount, limit):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="swap", args=[_prep(H, "swap"), key, [zfo, amount, limit], b""],
        app_references=[H.helper1_id], box_references=[*_boxes(H), *_empties(6 - len(_boxes(H)))],
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(500000))))


def _settle(H, g, currency):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="settleCurrency", args=[_prep(H, "settleCurrency"), currency], box_references=_empties(4),
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(3000))))


def _take(H, g, currency, amount, app, asa):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="take", args=[_prep(H, "take"), currency, H.sender, amount], app_references=[app],
        asset_references=[asa], box_references=_empties(4), note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(6000))))


def _donate(H, g, key, a0, a1):
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="donate", args=[_prep(H, "donate"), key, a0, a1, b""],
        app_references=[H.helper1_id], box_references=[*_boxes(H), *_empties(6 - len(_boxes(H)))],
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(8000))))


def _mint(H, g, id_, amount):  # ERC6909 claims mint (id = currency.toId())
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="mint", args=[_prep(H, "mint"), H.sender, id_, amount], box_references=_empties(4),
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(3000))))


def _burn(H, g, id_, amount):  # ERC6909 claims burn
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(
        method="burn", args=[_prep(H, "burn"), H.sender, id_, amount], box_references=_empties(4),
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(3000))))


def _measure(H, build):
    g = H.algorand.new_group(); _boost(H, g); build(g); g.build()
    req = _m.SimulateRequest(txn_groups=[], allow_unnamed_resources=True, extra_opcode_budget=320000,
        exec_trace_config=_m.SimulateTraceConfig(enable=True, scratch_change=True))
    grp = g._atc.simulate(H.algod, req).simulate_response["txn-groups"][0]
    fa = (grp.get("failed-at") or [len(grp["txn-results"]) - 1])[0]
    tr = grp["txn-results"][fa].get("exec-trace", {}).get("approval-program-trace", [])
    slots = {}
    for u in tr:
        for sc in u.get("scratch-changes", []):
            nv = sc.get("new-value", {})
            v = nv.get("uint", 0) if not nv.get("bytes") else int.from_bytes(base64.b64decode(nv["bytes"]), "big")
            slots[sc.get("slot")] = v
    buckets = {}
    for b in range(slots.get(6, 0)):
        buckets[slots.get(7 + 3 * b, 0)] = (slots.get(8 + 3 * b, 0), slots.get(9 + 3 * b, 0))
    return buckets


def _bal_of(H, t, addr):
    return int(t.tok.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[addr])).abi_return)


def _bal(H, t):
    return _bal_of(H, t, H.sender)


def _key(H, fee):
    return [H.c0, H.c1, fee, 60, ZERO_ADDR]


def _init(H, key):
    g = H.algorand.new_group(); _boost(H, g, 4)
    g.add_app_call_method_call(H.main_client.params.call(au.AppClientMethodCallParams(method="initialize",
        args=[_prep(H, "initialize"), key, INIT_SQRT], app_references=[H.helper1_id], box_references=_empties(6),
        note=_nxt(H), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.send()


def _seed(H, key):
    buckets = _measure(H, lambda g: (_unlock(H, g), _modliq(H, g, key, 60, 120, SEED_LIQ)))
    owe0, owe1 = buckets.get(_cid(H.c0), (0, 0))[0], buckets.get(_cid(H.c1), (0, 0))[0]
    assert owe0 > 0 and owe1 > 0, f"seed should owe both currencies, got {buckets}"
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _modliq(H, g, key, 60, 120, SEED_LIQ)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t0.asa, amount=owe0)); _settle(H, g, H.c0)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t1.asa, amount=owe1)); _settle(H, g, H.c1)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    return owe0, owe1


# ── tests: one shared pool (file order = lifecycle; remove-liquidity runs last) ──

def test_pool_seeded(H):
    """The fixture initialized + seeded the pool; it holds exactly the owed amounts."""
    assert _bal_of(H, H.t0, H.main_addr) == H.owe0
    assert _bal_of(H, H.t1, H.main_addr) == H.owe1


def test_swap_zeroForOne(H):
    buckets = _measure(H, lambda g: (_unlock(H, g), _swap(H, g, H.pool, True, -2000, LIMIT_DOWN)))
    amt_in, amt_out = buckets.get(_cid(H.c0), (0, 0))[0], buckets.get(_cid(H.c1), (0, 0))[1]
    assert amt_in == 2000 and 1000 < amt_out < 4000
    b1 = _bal(H, H.t1)
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _swap(H, g, H.pool, True, -2000, LIMIT_DOWN)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t0.asa, amount=amt_in)); _settle(H, g, H.c0)
    _take(H, g, H.c1, amt_out, H.t1.app, H.t1.asa)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal(H, H.t1) - b1 == amt_out  # user received the c1 output


def test_swap_oneForZero(H):
    buckets = _measure(H, lambda g: (_unlock(H, g), _swap(H, g, H.pool, False, -2000, LIMIT_UP)))
    amt_in, amt_out = buckets.get(_cid(H.c1), (0, 0))[0], buckets.get(_cid(H.c0), (0, 0))[1]
    assert amt_in == 2000 and 1000 < amt_out < 4000
    b0 = _bal(H, H.t0)
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _swap(H, g, H.pool, False, -2000, LIMIT_UP)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t1.asa, amount=amt_in)); _settle(H, g, H.c1)
    _take(H, g, H.c0, amt_out, H.t0.app, H.t0.asa)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal(H, H.t0) - b0 == amt_out


def test_cross_currency_cheat_reverts(H):
    """take c1 but settle c0 (equal amounts) — the conflated model passed this; per-currency rejects it."""
    with pytest.raises(Exception):
        g = H.algorand.new_group(); _boost(H, g); _unlock(H, g)
        _take(H, g, H.c1, 100, H.t1.app, H.t1.asa)
        g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t0.asa, amount=100)); _settle(H, g, H.c0)
        g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})


def test_donate(H):
    """donate(amount0, amount1) adds both currencies to the pool's reserves (user pays them in)."""
    a0 = a1 = 1000
    p0, p1 = _bal_of(H, H.t0, H.main_addr), _bal_of(H, H.t1, H.main_addr)
    buckets = _measure(H, lambda g: (_unlock(H, g), _donate(H, g, H.pool, a0, a1)))
    owe0, owe1 = buckets.get(_cid(H.c0), (0, 0))[0], buckets.get(_cid(H.c1), (0, 0))[0]
    assert owe0 == a0 and owe1 == a1, f"donate should owe exactly the donated amounts, got {buckets}"
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _donate(H, g, H.pool, a0, a1)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t0.asa, amount=a0)); _settle(H, g, H.c0)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t1.asa, amount=a1)); _settle(H, g, H.c1)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal_of(H, H.t0, H.main_addr) == p0 + a0 and _bal_of(H, H.t1, H.main_addr) == p1 + a1


def test_erc6909_claims(H):
    """Take a swap output as ERC6909 claims (mint) instead of real tokens, then redeem (burn + take)."""
    toid1 = int.from_bytes(algosdk.encoding.decode_address(H.c1)[12:], "big")  # currency.toId()
    buckets = _measure(H, lambda g: (_unlock(H, g), _swap(H, g, H.pool, True, -2000, LIMIT_DOWN)))
    amt_in, amt_out = buckets.get(_cid(H.c0), (0, 0))[0], buckets.get(_cid(H.c1), (0, 0))[1]
    assert amt_in == 2000 and amt_out > 0
    b1 = _bal(H, H.t1)
    # 1) swap output -> ERC6909 claims (mint), NOT a real take
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _swap(H, g, H.pool, True, -2000, LIMIT_DOWN)
    g.add_asset_transfer(au.AssetTransferParams(sender=H.sender, receiver=H.main_addr, asset_id=H.t0.asa, amount=amt_in)); _settle(H, g, H.c0)
    _mint(H, g, toid1, amt_out)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal(H, H.t1) == b1, "output went to ERC6909 claims; real c1 unchanged"
    # 2) redeem: burn the claims + take the real token
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _burn(H, g, toid1, amt_out)
    _take(H, g, H.c1, amt_out, H.t1.app, H.t1.asa)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal(H, H.t1) == b1 + amt_out, "claims redeemed for the real token"


@pytest.mark.xfail(reason="modliq with a NEGATIVE liquidityDelta (remove) hits an int128 sign "
                   "assert (biguint vs 2^127) in an untested codegen path; the add path (positive) "
                   "works with the identical box setup, so it's negative-delta int128 handling, not "
                   "box-refs. Needs the remove counterpart to the add-path int fixes (#49/#50).")
def test_remove_liquidity(H):
    """Removing the seeded liquidity returns both currencies to the user (runs LAST)."""
    buckets = _measure(H, lambda g: (_unlock(H, g), _modliq(H, g, H.pool, 60, 120, -SEED_LIQ)))
    got0, got1 = buckets.get(_cid(H.c0), (0, 0))[1], buckets.get(_cid(H.c1), (0, 0))[1]
    assert got0 > 0 and got1 > 0, f"remove should return both currencies, got {buckets}"
    p0, p1 = _bal(H, H.t0), _bal(H, H.t1)
    g = H.algorand.new_group(); _boost(H, g); _unlock(H, g); _modliq(H, g, H.pool, 60, 120, -SEED_LIQ)
    _take(H, g, H.c0, got0, H.t0.app, H.t0.asa)
    _take(H, g, H.c1, got1, H.t1.app, H.t1.asa)
    g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    assert _bal(H, H.t0) - p0 == got0 and _bal(H, H.t1) - p1 == got1  # user got the liquidity back
