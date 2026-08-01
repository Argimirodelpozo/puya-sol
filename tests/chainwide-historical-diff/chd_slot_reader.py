"""Slot-mode AVM storage reader — for `--evm-storage-layout` replays.

Under the mode, contract storage IS solc's slot layout, backed by boxes:
  "p:" ++ itob(slot // 64)   → 2048-byte page (64 dense slots < 2^16)
  "s:" ++ slot32             → one 32-byte box per keccak-derived slot

So the AVM leg can read storage exactly the way the EVM leg does — walk solc's
storageLayout for scalars, forward-derive mapping entry slots from the
registry's keys — over a slot→word map reconstructed from the app's boxes.
The output shapes match evm_leg's readers one-for-one, so differ.py compares
them unchanged. The only asymmetry is ADDRESS content: the AVM leg stores
32-byte AVM addresses (full-slot rule), and address-keyed mapping slots hash
the 32-byte AVM key — both handled here with the AVM-side syms/fold.
"""
from __future__ import annotations

import base64
import re

from Crypto.Hash import keccak as _keccak_mod


def _kec(b: bytes) -> bytes:
    k = _keccak_mod.new(digest_bits=256)
    k.update(b)
    return k.digest()


SLOTS_PER_PAGE = 64


def read_slot_map(algod, app_id) -> dict:
    """{slot(int): 32-byte word} from the app's page + sparse boxes."""
    out = {}
    try:
        boxes = algod.application_boxes(app_id).get("boxes") or []
    except Exception:
        return out
    for b in boxes:
        name = base64.b64decode(b["name"])
        try:
            raw = base64.b64decode(
                (algod.application_box_by_name(app_id, name) or {}).get("value") or "")
        except Exception:
            continue
        if name.startswith(b"p:") and len(name) == 10:
            page = int.from_bytes(name[2:], "big")
            for j in range(len(raw) // 32):
                w = raw[j * 32:(j + 1) * 32]
                if any(w):
                    out[page * SLOTS_PER_PAGE + j] = w
        elif name.startswith(b"s:") and len(name) == 34:
            if any(raw):
                out[int.from_bytes(name[2:], "big")] = raw.rjust(32, b"\0")
    return out


def read_slot_storage(slotmap: dict, layout: dict, syms: dict, fold, calls):
    """Mirror evm_leg's read_scalars/read_maps over the slot map.

    `syms` is {symbol: 32-byte AVM address content} (senders as real account
    keys, arg-only addresses as bytes(12)+content20 — exactly what the replay
    passed into calls, hence what the contract hashed into mapping slots).
    Returns {"scalars": …, "maps": {…, "__declared__": …,
    "__unattributed_boxes__": N}} — differ.py-compatible.
    """
    types = layout.get("types") or {}
    seen: set = set()

    def word(slot_int: int) -> bytes:
        seen.add(slot_int)
        return slotmap.get(slot_int, bytes(32))

    def dec(raw: bytes, label: str, full_word: bytes | None = None):
        """evm_leg._decode_slot_bytes twin. For a 20-byte address window the
        AVM word may hold a FULL 32-byte AVM address (full-slot widening) —
        try folding the whole word first, then the 12-zeros+trailing-20 form."""
        if label == "address" or label.startswith("contract "):
            if full_word is not None and len(raw) == 20:
                whole = fold("0x" + full_word.hex())
                if not str(whole).startswith("?"):
                    return whole
            padded = raw.rjust(32, b"\0")
            return fold("0x" + padded.hex())
        if label == "bool":
            return bool(raw[-1] if raw else 0)
        if label.startswith("uint"):
            return int.from_bytes(raw, "big")
        if label.startswith("int"):
            return int.from_bytes(raw, "big", signed=True)
        if label.startswith("bytes"):
            return "0x" + raw.hex()
        if label.startswith("enum"):
            return int.from_bytes(raw, "big")
        return "0x" + raw.hex()

    # ── scalars (inplace, non-aggregate — same selection as evm_leg) ──────
    scalars_out = {}
    for e in layout.get("storage") or []:
        t = types.get(e["type"], {})
        label = t.get("label", "")
        if t.get("encoding") != "inplace":
            continue
        if label.startswith(("mapping", "struct")) or label.endswith("]"):
            continue
        slot, off = int(e["slot"]), int(e.get("offset", 0))
        nb = int(t.get("numberOfBytes", 32))
        w = word(slot)
        scalars_out[e["label"]] = dec(w[32 - off - nb:32 - off], label, w)

    # ── string/bytes state vars: attribute their slots for coverage. The
    # scalar readers on BOTH legs skip dynamic encodings (compared via getter
    # snapshots instead), but the slot map ENUMERATES their nonzero slots —
    # mark the length word and any long-form data chunks as seen so they don't
    # read as blind spots.
    for e in layout.get("storage") or []:
        t = types.get(e["type"], {})
        if t.get("encoding") != "bytes":
            continue
        slot = int(e["slot"])
        w = word(slot)
        if w[-1] % 2 == 1:                       # long form: 2*len+1
            n = (int.from_bytes(w, "big") - 1) // 2
            base = int.from_bytes(_kec(slot.to_bytes(32, "big")), "big")
            for i in range((n + 31) // 32):
                seen.add(base + i)

    # ── mappings (address / bytes32 keys; scalar/struct/array/nested values) ──
    def _is_addr_mapping(label):
        return bool(re.match(r"^mapping\(address\b", label or ""))

    def _is_b32_mapping(label):
        return bool(re.match(r"^mapping\(bytes32\b", label or ""))

    def _value_shape(tid):
        vt = types.get(tid, {})
        label, enc = vt.get("label", ""), vt.get("encoding")
        if _is_addr_mapping(label):
            return ("mapping", vt)
        if enc == "inplace" and vt.get("members"):
            return ("struct", vt)
        if enc == "dynamic_array":
            return ("array", vt)
        if enc == "inplace":
            return ("scalar", vt)
        return (None, vt)

    b32_keys = []
    for c in calls:
        for a in c.get("args") or []:
            if isinstance(a, dict) and set(a) == {"__b__"} and len(a["__b__"]) == 64:
                if a["__b__"] not in b32_keys:
                    b32_keys.append(a["__b__"])

    maps = []
    declared = []
    for e in layout.get("storage") or []:
        t = types.get(e["type"], {})
        label = t.get("label", "")
        if label.startswith("mapping"):
            declared.append(e["label"])
        if _is_b32_mapping(label):
            kind, vt = _value_shape(t.get("value"))
            if kind in ("scalar", "struct", "array"):
                maps.append((e["label"], int(e["slot"]), "b32", (kind, vt)))
            continue
        if not _is_addr_mapping(label):
            continue
        kind, vt = _value_shape(t.get("value"))
        if kind == "mapping":
            k2, vt2 = _value_shape(vt.get("value"))
            if k2 == "scalar":
                maps.append((e["label"], int(e["slot"]), 2, ("scalar", vt2)))
        elif kind in ("scalar", "struct", "array"):
            maps.append((e["label"], int(e["slot"]), 1, (kind, vt)))

    def read_at(slot_int, shape):
        kind, vt = shape
        if kind == "scalar":
            raw = word(slot_int)
            if not any(raw):
                return None
            return dec(raw, vt.get("label", "uint256"), raw)
        if kind == "struct":
            vals, seen_any = [], False
            for m in vt.get("members") or []:
                mt = types.get(m["type"], {})
                nb = int(mt.get("numberOfBytes", 32))
                off = int(m.get("offset", 0))
                w = word(slot_int + int(m.get("slot", 0)))
                if any(w):
                    seen_any = True
                vals.append(dec(w[32 - off - nb:32 - off],
                                mt.get("label", "uint256"), w))
            return vals if seen_any else None
        if kind == "array":
            n = int.from_bytes(word(slot_int), "big")
            if not n or n > 512:
                return None if not n else f"<{n} elements>"
            base = int.from_bytes(_kec(slot_int.to_bytes(32, "big")), "big")
            et = types.get(vt.get("base"), {})
            esz = int(et.get("numberOfBytes", 32))
            ekind = ("struct" if et.get("members") else "scalar", et)
            out_l = []
            for i in range(n):
                per = max(1, (esz + 31) // 32)
                out_l.append(read_at(base + i * per, ekind))
            return out_l
        return None

    # nested-pair candidates from the calls themselves (mirror evm_leg)
    from chd_common import symbol as _symbol

    def _syms_in(v):
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            yield _symbol(v["__addr__"])
        elif isinstance(v, list):
            for x in v:
                yield from _syms_in(x)

    pair_partners: dict = {}
    for c in calls:
        if not c.get("sender") or not c.get("args"):
            continue
        s_sym = _symbol(c["sender"]["__addr__"])
        seen_args = [t for a in c["args"] for t in _syms_in(a)]
        for a_sym in [s_sym] + seen_args:
            partners = pair_partners.setdefault(a_sym, set())
            partners.add(a_sym)
            for t2 in [s_sym] + seen_args:
                partners.add(t2)

    maps_out = {"__declared__": sorted(declared)}
    for name, slot, depth, vshape in maps:
        got = {}
        if depth == "b32":
            for kh in b32_keys:
                s1 = _kec(bytes.fromhex(kh) + slot.to_bytes(32, "big"))
                v = read_at(int.from_bytes(s1, "big"), vshape)
                if v is not None:
                    got["0x" + kh] = v
            maps_out[name] = got
            continue
        for sym, k in syms.items():
            if not isinstance(k, (bytes, bytearray)) or len(k) != 32:
                continue
            s1 = _kec(bytes(k) + slot.to_bytes(32, "big"))
            if depth == 1:
                v = read_at(int.from_bytes(s1, "big"), vshape)
                if v is not None:
                    got[sym] = v
            else:
                for sym2 in pair_partners.get(sym, ()):
                    k2 = syms.get(sym2)
                    if not isinstance(k2, (bytes, bytearray)) or len(k2) != 32:
                        continue
                    s2 = _kec(bytes(k2) + s1)
                    v = read_at(int.from_bytes(s2, "big"), vshape)
                    if v is not None:
                        got[f"{sym}->{sym2}"] = v
        maps_out[name] = got

    # Coverage: slots present in a box that NO reader looked at. This is the
    # slot-mode mirror of the box-attribution check — and strictly stronger
    # (dense pages enumerate every nonzero slot, not just derivable names).
    maps_out["__unattributed_boxes__"] = len(set(slotmap) - seen)
    return {"scalars": scalars_out, "maps": maps_out}
