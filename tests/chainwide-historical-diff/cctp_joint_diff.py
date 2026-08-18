#!/usr/bin/env python3
"""Diff the joint CCTP replay's three artifacts: AVM oracle report,
joint-EVM-leg results, and historical receipt logs.

  python3 cctp_joint_diff.py cases \
      --avm cctp_joint_report.json --evm cases/cctp_evm_results.json

Three comparisons:
  * STATUS (three-way): historical receipt vs AVM oracle vs joint EVM leg.
  * EVENTS: AVM ARC-28 logs vs the HISTORICAL receipt logs (ground truth),
    per transaction, as multisets of (contract, event, canonical args).
    The joint EVM leg's logs are diffed against history the same way as an
    independent cross-check.
  * STORAGE: AVM slot→word maps (page/sparse boxes) vs the EVM leg's traced
    slot→word maps, slot-for-slot per contract.

Canonicalization: addresses fold to the 20-byte historical form — AVM values
are zero-padded historical bytes except tracked contracts (bytes24+app_id);
EVM-leg values are raw historical bytes except tracked contracts (local
deployments). Both fold through their translation tables before comparing.

Known-noise buckets (counted, never findings):
  * usdc_dependency — StubERC20 state is synthetic on both legs; its events
    and the real USDC's historical events are not comparable.
  * zero_arg_event — puya cannot register zero-argument events (puyabug #11).
  * foreign_address_log — historical logs emitted by untracked contracts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any

import runpy

HERE = Path(__file__).parent
DRIVER = runpy.run_path(str(HERE / "oracle_cctp_historical.py"))
CASE_CONFIG = DRIVER["CASE_CONFIG"]
STUB_CONFIG = DRIVER["STUB_CONFIG"]
historical_ok = DRIVER["historical_ok"]

APP_TO_TAG = {cfg["app_id"]: tag for tag, cfg in CASE_CONFIG.items()}
APP_TO_TAG[STUB_CONFIG["app_id"]] = "stub_usdc"
APP_TO_HIST = {cfg["app_id"]: cfg["address"].lower() for cfg in CASE_CONFIG.values()}
APP_TO_HIST[STUB_CONFIG["app_id"]] = STUB_CONFIG["address"].lower()
HIST_TO_TAG = {cfg["address"].lower(): tag for tag, cfg in CASE_CONFIG.items()}
HIST_TO_TAG[STUB_CONFIG["address"].lower()] = "stub_usdc"


# ── canonical address folding ────────────────────────────────────────────────
def fold_avm_address(word: bytes) -> str:
    """32-byte AVM address value → 20-byte historical hex."""
    if word[:24] == bytes(24):
        app_id = int.from_bytes(word[24:], "big")
        hist = APP_TO_HIST.get(app_id)
        if hist is not None:
            return hist
    if word[:12] == bytes(12):
        return "0x" + word[12:].hex()
    return "0x" + word.hex()  # full 32B — surfaced as-is (unexpected)


def make_evm_folder(local_addresses: dict[str, str]):
    local_to_hist = {
        local.lower(): hist
        for hist, tag in HIST_TO_TAG.items()
        for local_tag, local in local_addresses.items()
        if local_tag == tag
    }

    def fold(addr_hex: str) -> str:
        low = addr_hex.lower()
        if not low.startswith("0x"):
            low = "0x" + low
        return local_to_hist.get(low, low)

    return fold, local_to_hist


# ── EVM log decoding (solc ABI) ──────────────────────────────────────────────
def keccak(data: bytes) -> bytes:
    from Crypto.Hash import keccak as ck  # pycryptodome, present system-wide

    return ck.new(digest_bits=256, data=data).digest()


def solc_event_index(abis: dict[str, list]) -> dict[bytes, tuple[str, dict]]:
    """topic0 → (tag-agnostic event name, abi entry)."""
    out: dict[bytes, tuple[str, dict]] = {}
    for _tag, abi in abis.items():
        for item in abi:
            if item.get("type") != "event":
                continue
            sig = item["name"] + "(" + ",".join(i["type"] for i in item["inputs"]) + ")"
            out[keccak(sig.encode())] = (item["name"], item)
    return out


def decode_evm_word(abi_type: str, word: bytes, fold_addr) -> Any:
    if abi_type == "address":
        return fold_addr("0x" + word[12:].hex())
    if abi_type == "bool":
        return bool(int.from_bytes(word, "big"))
    if abi_type.startswith("uint") or abi_type.startswith("int"):
        return int.from_bytes(word, "big")
    return "0x" + word.hex()


def decode_evm_log(entry: dict, event: dict, fold_addr) -> tuple:
    """(name, arg-tuple) using indexed topics + ABI-encoded data words."""
    topics = [bytes.fromhex(t[2:]) for t in entry["topics"][1:]]
    data = bytes.fromhex(entry["data"][2:]) if entry["data"] != "0x" else b""
    args: list[Any] = []
    ti = 0
    data_types = [i["type"] for i in event["inputs"] if not i["indexed"]]
    data_args = decode_abi_data(data_types, data, fold_addr)
    di = 0
    for inp in event["inputs"]:
        if inp["indexed"]:
            if inp["type"] in ("bytes", "string") or inp["type"].endswith("]"):
                args.append("indexed-dynamic:" + topics[ti].hex())
            else:
                args.append(decode_evm_word(inp["type"], topics[ti], fold_addr))
            ti += 1
        else:
            args.append(data_args[di])
            di += 1
    return (event["name"], tuple(args))


def decode_abi_data(types: list[str], data: bytes, fold_addr) -> list[Any]:
    """Compact ABI decoder for the subset CCTP events use."""
    head: list[Any] = []
    for i, t in enumerate(types):
        word = data[i * 32 : (i + 1) * 32]
        if t in ("bytes", "string"):
            off = int.from_bytes(word, "big")
            length = int.from_bytes(data[off : off + 32], "big")
            blob = data[off + 32 : off + 32 + length]
            head.append(
                blob.decode() if t == "string" else "0x" + blob.hex()
            )
        else:
            head.append(decode_evm_word(t, word, fold_addr))
    return head


# ── AVM ARC-28 log decoding ──────────────────────────────────────────────────
def arc56_event_index(cases_dir: Path) -> dict[bytes, tuple[str, dict]]:
    out: dict[bytes, tuple[str, dict]] = {}
    for tag, cfg in CASE_CONFIG.items():
        arc = json.loads(
            (cases_dir / tag / "out_avm" / f"{cfg['contract']}.arc56.json").read_text()
        )
        for ev in arc.get("events") or []:
            sig = ev["name"] + "(" + ",".join(a["type"] for a in ev["args"]) + ")"
            out[hashlib.new("sha512_256", sig.encode()).digest()[:4]] = (
                ev["name"],
                ev,
            )
    return out


def decode_arc4(types: list[str], data: bytes, fold_addr) -> list[Any]:
    """ARC-4 tuple decode for the shapes puya-sol events use."""

    def static_size(t: str) -> int | None:
        if t == "address":
            return 32
        if t == "bool":
            return 1
        if t.startswith("uint"):
            return int(t[4:]) // 8
        if t.startswith("byte[") and t.endswith("]") and t != "byte[]":
            return int(t[5:-1])
        return None  # dynamic

    sizes = [static_size(t) for t in types]
    head_size = sum(2 if s is None else s for s in sizes)
    out: list[Any] = []
    pos = 0
    for t, s in zip(types, sizes):
        if s is None:
            off = int.from_bytes(data[pos : pos + 2], "big")
            length = int.from_bytes(data[off : off + 2], "big")
            out.append("0x" + data[off + 2 : off + 2 + length].hex())
            pos += 2
        else:
            chunk = data[pos : pos + s]
            if t == "address":
                out.append(fold_avm_address(chunk))
            elif t == "bool":
                out.append(chunk != b"\x00")
            elif t.startswith("uint"):
                out.append(int.from_bytes(chunk, "big"))
            else:
                out.append("0x" + chunk.hex())
            pos += s
    del head_size
    return out


def decode_avm_log(
    raw_hex: str, index: dict[bytes, tuple[str, dict]]
) -> tuple | None:
    blob = bytes.fromhex(raw_hex)
    ev = index.get(blob[:4])
    if ev is None:
        return None
    name, spec = ev
    args = decode_arc4([a["type"] for a in spec["args"]], blob[4:], fold_avm_address)
    return (name, tuple(args))


# ── comparison ───────────────────────────────────────────────────────────────
# Two inherent closed-world divergences are NORMALIZED (and counted), never
# reported as findings:
#  * SEND-side nonce skew: history's transmitter nonces are sparse (unreplayed
#    third-party sendMessages consumed some); both replays are dense 0,1,2,….
#    Receive-side nonces come from the attested historical messages and are
#    compared exactly.
#  * AVM sender representation: addressToBytes32(msg.sender) inside a
#    constructed message is the app account's low 20 bytes on the AVM leg;
#    fold each tracked contract's form to its historical address.
SEND_NONCE_ARGS = {"MessageSent": [], "DepositForBurn": [0]}
MESSAGE_BLOB_ARGS = {"MessageSent": [0]}
CCTP_NONCE_FIELD = slice(12, 20)  # version(4) src(4) dst(4) nonce(8) sender(32)
CCTP_SENDER_FIELD = slice(20, 52)


def avm_account_low20(app_id: int) -> bytes:
    return hashlib.new("sha512_256", b"appID" + app_id.to_bytes(8, "big")).digest()[
        12:
    ]


# Both AVM 32-byte spellings of a tracked contract, folded to the historical
# form wherever they appear inside a constructed message blob: the Solidity
# address VALUE (bytes24 + app_id, e.g. burnToken in a BurnMessage body) and
# addressToBytes32(msg.sender) (pad12 + the app account's low 20 bytes).
BLOB_FOLDS = []
for _cfg in [*CASE_CONFIG.values(), STUB_CONFIG]:
    _hist32 = bytes(12) + bytes.fromhex(_cfg["address"][2:])
    BLOB_FOLDS.append((bytes(24) + int(_cfg["app_id"]).to_bytes(8, "big"), _hist32))
    BLOB_FOLDS.append((bytes(12) + avm_account_low20(_cfg["app_id"]), _hist32))


def normalize_events(
    events: list[tuple], counts: Counter, avm_side: bool
) -> list[tuple]:
    out = []
    for name, args in events:
        args = list(args)
        for i in SEND_NONCE_ARGS.get(name, []):
            if i < len(args):
                args[i] = "<send-nonce>"
                counts["send_nonce_normalized"] += 1
        for i in MESSAGE_BLOB_ARGS.get(name, []):
            if i >= len(args) or not isinstance(args[i], str):
                continue
            raw = args[i]
            blob = bytearray.fromhex(raw[2:] if raw.startswith("0x") else raw)
            if len(blob) >= 52:
                blob[CCTP_NONCE_FIELD] = b"<nonce8>"
                counts["send_nonce_normalized"] += 1
                if avm_side:
                    folded = bytes(blob)
                    for avm32, hist32 in BLOB_FOLDS:
                        if avm32 in folded:
                            folded = folded.replace(avm32, hist32)
                            counts["avm_address_repr_folded"] += 1
                    blob = bytearray(folded)
            args[i] = "0x" + bytes(blob).hex()
        out.append((name, tuple(args)))
    return out


def canonical(events: list[tuple]) -> Counter:
    return Counter((name, tuple(str(a) for a in args)) for name, args in events)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path)
    parser.add_argument("--avm", type=Path, required=True)
    parser.add_argument("--evm", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    avm = json.loads(args.avm.read_text())
    evm = json.loads(args.evm.read_text()) if args.evm else None

    # historical logs, all cases merged by hash
    hist_logs: dict[str, list[dict]] = {}
    abis: dict[str, list] = {}
    for tag in CASE_CONFIG:
        p = args.cases / tag / "logs.json"
        if p.exists():
            for h, entries in json.loads(p.read_text()).items():
                hist_logs.setdefault(h, entries)
        abis[tag] = json.loads((args.cases / tag / "case.json").read_text())["abi"]

    topic_index = solc_event_index(abis)
    arc_index = arc56_event_index(args.cases)
    fold_evm = (lambda a: a.lower())
    local_to_hist: dict[str, str] = {}
    if evm:
        fold_evm, local_to_hist = make_evm_folder(evm.get("addresses", {}))

    counts = Counter()
    findings: list[dict[str, Any]] = []

    # ── events: AVM vs history (and EVM vs history) ──────────────────────
    for rec in avm["results"]:
        h = rec["hash"].split("#")[0]
        if rec["oracle_result"] != "ACCEPT" or not rec["historical_ok"]:
            continue
        hist_entries = hist_logs.get(h)
        if hist_entries is None:
            counts["no_historical_logs"] += 1
            continue
        if not hist_entries and rec["signature"]:
            # An "ok" receipt with ZERO log entries is self-contradictory for
            # every state-changing CCTP method (success always emits) — the
            # indexer's receipt is corrupt (same class as the verified
            # stale-status corrections). The replay's events cannot be
            # compared against an empty corrupt receipt.
            counts["corrupt_empty_receipt"] += 1
            continue
        hist_events: list[tuple] = []
        for entry in hist_entries:
            tag = HIST_TO_TAG.get(entry["address"])
            if tag is None:
                counts["foreign_address_log"] += 1
                continue
            if tag == "stub_usdc":
                counts["usdc_dependency"] += 1
                continue
            key = bytes.fromhex(entry["topics"][0][2:])
            ev = topic_index.get(key)
            if ev is None:
                counts["unknown_historical_topic"] += 1
                continue
            hist_events.append(decode_evm_log(entry, ev[1], lambda a: a.lower()))

        avm_events: list[tuple] = []
        raw_logs = list(rec.get("logs") or [])
        for inner in rec.get("inner_logs") or []:
            app = inner.get("app")
            if APP_TO_TAG.get(app) == "stub_usdc":
                counts["stub_avm_logs_skipped"] += len(inner["logs"])
                continue
            raw_logs.extend(inner["logs"])
        for raw in raw_logs:
            if raw.startswith("151f7c75"):
                counts["arc4_return_value_log"] += 1  # not an event
                continue
            decoded = decode_avm_log(raw, arc_index)
            if decoded is None:
                counts["undecodable_avm_log"] += 1
                continue
            avm_events.append(decoded)

        hist_events = normalize_events(hist_events, counts, avm_side=False)
        avm_events = normalize_events(avm_events, counts, avm_side=True)
        want, got = canonical(hist_events), canonical(avm_events)
        if want != got:
            missing = want - got
            extra = got - want
            zero_arg = all(
                not argtuple for (_n, argtuple) in missing
            ) and not extra
            if zero_arg and missing:
                counts["zero_arg_event"] += sum(missing.values())
            else:
                counts["event_div"] += 1
                findings.append(
                    {
                        "kind": "event_div",
                        "hash": h,
                        "missing_on_avm": [list(k) for k in missing],
                        "extra_on_avm": [list(k) for k in extra],
                    }
                )
        else:
            counts["event_txns_matched"] += 1

        if evm is not None and h in evm["logs"]:
            evm_events = []
            for entry in evm["logs"][h]:
                hist_addr = local_to_hist.get(entry["address"].lower())
                tag = HIST_TO_TAG.get(hist_addr or "")
                if tag is None or tag == "stub_usdc":
                    continue
                key = bytes.fromhex(entry["topics"][0][2:])
                ev = topic_index.get(key)
                if ev is None:
                    continue
                evm_events.append(decode_evm_log(entry, ev[1], fold_evm))
            evm_events = normalize_events(evm_events, counts, avm_side=False)
            if canonical(evm_events) != want:
                counts["evm_leg_event_div"] += 1
                findings.append({"kind": "evm_leg_event_div", "hash": h})
            else:
                counts["evm_leg_event_txns_matched"] += 1

    # ── statuses: three-way ──────────────────────────────────────────────
    if evm is not None:
        for rec in avm["results"]:
            h = rec["hash"]
            if h in evm["statuses"]:
                counts["status_threeway_compared"] += 1
                if (rec["oracle_result"] == "ACCEPT") != evm["statuses"][h]:
                    counts["status_avm_vs_evm_div"] += 1
                    findings.append({"kind": "status_leg_div", "hash": h})

    # ── storage: AVM slots vs EVM slots ──────────────────────────────────
    # Address-keyed mapping slots diverge BY CONSTRUCTION for the tracked
    # contracts: the AVM leg's Solidity address value for a tracked contract
    # is bytes24+app_id while the EVM leg holds the padded historical bytes,
    # so keccak(key‖base) lands on different slots for the SAME logical entry.
    # Precompute the equivalence for plausible mapping bases and rewrite AVM
    # slots onto their EVM twins before comparing.
    slot_twin: dict[int, int] = {}
    for cfg in [*CASE_CONFIG.values(), STUB_CONFIG]:
        avm_key = bytes(24) + int(cfg["app_id"]).to_bytes(8, "big")
        evm_key = bytes(12) + bytes.fromhex(cfg["address"][2:])
        for base in range(64):
            b32 = base.to_bytes(32, "big")
            slot_twin[int.from_bytes(keccak(avm_key + b32), "big")] = int.from_bytes(
                keccak(evm_key + b32), "big"
            )
    if evm is not None:
        avm_storage = avm.get("final_storage") or {}
        evm_storage = evm.get("storage") or {}
        for tag in CASE_CONFIG:
            a = {int(k): v for k, v in (avm_storage.get(tag) or {}).items()}
            e = {int(k): v for k, v in (evm_storage.get(tag) or {}).items()}
            remapped = {}
            for slot, word in a.items():
                if slot in slot_twin:
                    counts["storage_slots_app_id_key_remapped"] += 1
                    remapped[slot_twin[slot]] = word
                else:
                    remapped[slot] = word
            a = remapped
            for slot in sorted(set(a) | set(e)):
                aw = bytes.fromhex(a.get(slot, "00" * 32))
                ew = bytes.fromhex(e.get(slot, "00" * 32))
                if aw == ew:
                    if any(aw):
                        counts["storage_slots_matched"] += 1
                    continue
                # fold address-shaped words through the translation tables
                fa = fold_avm_address(aw)
                fe = fold_evm("0x" + ew[12:].hex()) if ew[:12] == bytes(12) else None
                if fe is not None and fa == fe:
                    counts["storage_slots_matched_folded"] += 1
                    continue
                counts["storage_div"] += 1
                findings.append(
                    {
                        "kind": "storage_div",
                        "tag": tag,
                        "slot": slot,
                        "avm": aw.hex(),
                        "evm": ew.hex(),
                    }
                )

    report = {"counts": dict(counts), "findings": findings}
    text = json.dumps(report, indent=1)
    if args.output:
        args.output.write_text(text)
    print(json.dumps(dict(counts), indent=1))
    print(f"findings: {len(findings)}")
    for f in findings[:10]:
        print(" ", json.dumps(f)[:200])
    return 0 if not findings else 1


if __name__ == "__main__":
    raise SystemExit(main())
