"""Storage reader for AVM replays compiled with ``--evm-storage-layout``.

The physical boxes reconstruct a slot→word map. Logical traversal is delegated
to the same recursive solc-layout reader used by the EVM leg, so slot mode no
longer maintains its own address/bytes32/depth-specific implementation.
"""
from __future__ import annotations

import base64

from Crypto.Hash import keccak as _keccak_mod

from chd_common import bytes32_mapping_key_candidates
from chd_storage import (EvmStorageReader, KeyCandidate, KeyEvidence,
                         evm_key_bytes)


def _kec(data: bytes) -> bytes:
    digest = _keccak_mod.new(digest_bits=256)
    digest.update(data)
    return digest.digest()


SLOTS_PER_PAGE = 64


def read_slot_map(algod, app_id) -> dict[int, bytes]:
    """Reconstruct nonzero EVM words from dense-page and sparse-slot boxes."""
    out = {}
    try:
        boxes = algod.application_boxes(app_id).get("boxes") or []
    except Exception:
        return out
    for item in boxes:
        name = base64.b64decode(item["name"])
        try:
            raw = base64.b64decode(
                (algod.application_box_by_name(app_id, name) or {}).get("value") or "")
        except Exception:
            continue
        if name.startswith(b"p:") and len(name) == 10:
            page = int.from_bytes(name[2:], "big")
            for index in range(len(raw) // 32):
                word = raw[index * 32:(index + 1) * 32]
                if any(word):
                    out[page * SLOTS_PER_PAGE + index] = word
        elif name.startswith(b"s:") and len(name) == 34 and any(raw):
            out[int.from_bytes(name[2:], "big")] = raw.rjust(32, b"\0")[-32:]
    return out


def read_slot_storage(slotmap: dict[int, bytes], layout: dict, syms: dict,
                      fold, calls, fns=None) -> dict:
    """Walk the reconstructed words through the shared recursive reader."""
    extras = bytes32_mapping_key_candidates(calls, fns or {}, _kec)
    evidence = KeyEvidence(calls, fns or {}, syms, extras)

    def slot_mode_key(candidate: KeyCandidate, type_doc: dict) -> bytes:
        label = str(type_doc.get("label") or "")
        # Address mapping KEYS live in the 160-bit namespace (bzero12 ++
        # low-20) — the compiler's EVM-faithful keccak preimage. The old
        # full-32-byte form predates the namespace work and orphaned every
        # derived entry whose symbol carried a raw AVM account (the creator/
        # dep symbols: AccessControl _roles member misses). Identity-form
        # candidates pass through unchanged.
        if label == "address" or label.startswith("contract "):
            raw = bytes(candidate.value)
            return bytes(12) + raw[-20:]
        return evm_key_bytes(candidate, type_doc, _kec)

    reader = EvmStorageReader(
        layout, lambda slot: slotmap.get(slot, bytes(32)), evidence, _kec,
        written_slots=set(slotmap), mapping_key_encoder=slot_mode_key,
        full_word_addresses=True)
    storage = reader.read(fold)
    storage["maps"]["__unattributed_boxes__"] = len(
        set(slotmap) - set(reader.seen))
    storage["coverage"] = {
        "slots_total": len(slotmap),
        "typed_slots": len(set(slotmap) & set(reader.seen)),
        "unattributed": len(set(slotmap) - set(reader.seen)),
    }
    return storage
