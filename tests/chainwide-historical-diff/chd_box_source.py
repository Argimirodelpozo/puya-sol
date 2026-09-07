"""Pluggable box sources for the native (default-mode) and slot-mode readers.

`chd_storage.NativeStorageReader` and `chd_slot_reader.read_slot_storage` walk
an app's boxes through a plain ``Mapping[bytes, bytes]`` (box name → content).
Where that mapping comes from is this module's job, so the readers — and the
holder-coordinate root check they carry — run the same way over LocalNet
(algod) and over avm-prover's oracle (a carried ``boxes_after`` state), with
no chain client in the loop for the latter.
"""
from __future__ import annotations

import base64
from collections.abc import Mapping
from typing import Iterator

SLOTS_PER_PAGE = 64


class BoxSource(Mapping):
    """Read-only ``name → content`` view over one app's boxes."""

    def __init__(self, values: dict[bytes, bytes]):
        self._values = dict(values)

    def __getitem__(self, name: bytes) -> bytes:
        return self._values[name]

    def __iter__(self) -> Iterator[bytes]:
        return iter(self._values)

    def __len__(self) -> int:
        return len(self._values)

    def snapshot(self) -> dict[bytes, bytes]:
        return dict(self._values)


class DictBoxSource(BoxSource):
    """A box set already in hand (tests, synthetic fixtures)."""


class AlgodBoxSource(BoxSource):
    """Every box of ``app_id`` fetched through algod — the LocalNet lane.

    Enumeration failure propagates (the caller reports ``__error__``); a box
    whose value cannot be read is skipped, exactly as the per-box loop this
    replaces did.
    """

    def __init__(self, algod, app_id: int):
        names = [base64.b64decode(item["name"])
                 for item in (algod.application_boxes(app_id).get("boxes") or [])]
        values: dict[bytes, bytes] = {}
        for name in names:
            try:
                values[name] = base64.b64decode(
                    (algod.application_box_by_name(app_id, name) or {}).get("value") or "")
            except Exception:
                continue
        super().__init__(values)


class OracleBoxSource(BoxSource):
    """Boxes from avm-prover oracle state: ``boxes_after`` / ``OracleState.boxes``
    entries (``{"key": hex, "bytes": hex[, "app": id]}``).

    ``app`` selects a foreign app's boxes; None keeps the current app's (entries
    without an ``app`` field, or ``app`` 0).
    """

    def __init__(self, entries, app: int | None = None):
        values: dict[bytes, bytes] = {}
        for item in entries or []:
            owner = int(item.get("app") or 0)
            if app is None and owner not in (0, 9001):
                continue
            if app is not None and owner != app:
                continue
            values[bytes.fromhex(item["key"])] = bytes.fromhex(item.get("bytes") or "")
        super().__init__(values)


def slot_map_from_boxes(box_values: Mapping) -> dict[int, bytes]:
    """``--evm-storage-layout`` physical boxes → nonzero ``slot → 32-byte word``.

    Dense pages ``"p:" ++ itob(slot // 64)`` hold 64 words; sparse
    ``"s:" ++ slot32`` boxes hold one keccak-derived slot each. Same decoding
    as ``chd_slot_reader.read_slot_map``, over any box mapping.
    """
    out: dict[int, bytes] = {}
    for name, raw in box_values.items():
        if name.startswith(b"p:") and len(name) == 10:
            page = int.from_bytes(name[2:], "big")
            for index in range(len(raw) // 32):
                word = raw[index * 32:(index + 1) * 32]
                if any(word):
                    out[page * SLOTS_PER_PAGE + index] = word
        elif name.startswith(b"s:") and len(name) == 34 and any(raw):
            out[int.from_bytes(name[2:], "big")] = raw.rjust(32, b"\0")[-32:]
    return out
