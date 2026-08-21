"""Type-directed storage coverage for chainwide differential replays.

The two chains use different physical storage models, but both expose enough
type information to walk the same logical tree:

* solc ``storageLayout`` describes Solidity containers recursively;
* ARC-56 names native AVM roots and the ARC4 types stored in their boxes.

This module owns the shared key evidence and the recursive EVM-layout reader.
The native AVM adapter lives in :mod:`avm_leg` because it also needs ARC-56
decoding, but it consumes the same ``KeyEvidence`` and path labels.
"""
from __future__ import annotations

import itertools
import json
import re
from dataclasses import dataclass
from typing import Any, Callable, Iterable


@dataclass(frozen=True)
class KeyCandidate:
    label: str
    value: Any
    contexts: frozenset[int] = frozenset()


def canonical_abi_type(spec: dict) -> str:
    typ = str(spec.get("type") or "")
    if typ.startswith("tuple"):
        body = "(" + ",".join(canonical_abi_type(c)
                               for c in spec.get("components") or []) + ")"
        return body + typ[len("tuple"):]
    return typ


def _array_element_spec(spec: dict) -> dict | None:
    typ = str(spec.get("type") or "")
    match = re.match(r"^(.*)\[(\d*)\]$", typ)
    if not match:
        return None
    return {**spec, "type": match.group(1)}


def _marker_key(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


class KeyEvidence:
    """Bounded, typed key candidates derived from replay evidence.

    Candidates are collected recursively from calldata rather than from a
    fixed mapping depth or a contract-specific list.  ``syms`` supplies the
    chain-local bytes for address symbols, while labels remain identical on
    both legs. Runtime values discovered in arrays/structs can be fed back into
    the pool, which is what lets a set's value array seed its position map.
    """

    def __init__(self, calls: list[dict], fns: dict, syms: dict[str, bytes],
                 bytes32_extras: Iterable[str] = ()):
        self.calls = calls or []
        self.fns = fns or {}
        self.syms = syms
        self._numbers: dict[int, set[int]] = {}
        self._blobs: dict[int, set[bytes]] = {}
        self._strings: dict[str, set[int]] = {}
        self._address_contexts: dict[str, set[int]] = {
            label: set() for label in syms
        }
        self._collect_calls()
        for item in bytes32_extras:
            try:
                raw = bytes.fromhex(str(item).removeprefix("0x"))
            except ValueError:
                continue
            if len(raw) == 32:
                self._blobs.setdefault(32, set()).add(raw)

    def _collect_calls(self) -> None:
        for index, call in enumerate(self.calls):
            sender = call.get("sender") or {}
            if isinstance(sender, dict) and set(sender) == {"__addr__"}:
                from chd_common import symbol
                self._address_contexts.setdefault(
                    symbol(sender["__addr__"]), set()).add(index)
            inputs = ((self.fns.get(call.get("sig")) or {}).get("inputs") or [])
            for value, spec in zip(call.get("args") or [], inputs):
                self._collect_value(value, spec, index)

    def _collect_value(self, value: Any, spec: dict, context: int) -> None:
        elem = _array_element_spec(spec)
        if elem is not None:
            for item in value or []:
                self._collect_value(item, elem, context)
            return
        typ = str(spec.get("type") or "")
        if typ == "tuple":
            for item, component in zip(value or [], spec.get("components") or []):
                self._collect_value(item, component, context)
            return
        if typ == "address" and isinstance(value, dict) \
                and set(value) == {"__addr__"}:
            from chd_common import symbol
            self._address_contexts.setdefault(
                symbol(value["__addr__"]), set()).add(context)
            return
        if re.fullmatch(r"u?int\d*", typ) and type(value) is int:
            self._numbers.setdefault(value, set()).add(context)
            return
        if typ == "bool" and isinstance(value, bool):
            self._numbers.setdefault(int(value), set()).add(context)
            return
        if typ.startswith("bytes") and isinstance(value, dict) \
                and set(value) == {"__b__"}:
            try:
                raw = bytes.fromhex(value["__b__"])
            except ValueError:
                return
            self._blobs.setdefault(len(raw), set()).add(raw)
            return
        if typ in ("string", "bytes") and isinstance(value, str):
            self._strings.setdefault(value, set()).add(context)

    @staticmethod
    def _kind(type_doc: dict) -> tuple[str, int | None]:
        label = str(type_doc.get("label") or "")
        if label == "address" or label.startswith("contract "):
            return "address", 20
        match = re.match(r"^uint(\d*)", label)
        if match:
            return "uint", int(match.group(1) or 256)
        match = re.match(r"^int(\d*)", label)
        if match:
            return "int", int(match.group(1) or 256)
        match = re.match(r"^bytes(\d+)$", label)
        if match:
            return "bytes", int(match.group(1))
        if label == "bool":
            return "bool", 8
        if label.startswith("enum "):
            return "uint", int(type_doc.get("numberOfBytes", 1)) * 8
        if label == "string":
            return "string", None
        if label == "bytes":
            return "dynamic-bytes", None
        return "unknown", None

    def address_label(self, raw: bytes) -> str | None:
        """Symbol for a 32-byte word holding an address, or None.

        Matches the registry bytes themselves AND their low-20 narrowing, since
        that is the form an AVM account takes once it has passed through a
        Solidity `address`. Requiring an exact 32-byte hit left every
        sender-held value rendering as raw hex while the EVM leg showed a
        symbol — two spellings of one account, reported as a divergence.
        """
        raw = bytes(raw)
        if len(raw) != 32:
            return None
        narrowed = raw[:12] == b"\0" * 12
        for label, value in self.syms.items():
            value = bytes(value)
            if value == raw or (narrowed and value[-20:] == raw[-20:]):
                return label
        return None

    def candidates(self, type_doc: dict) -> list[KeyCandidate]:
        kind, width = self._kind(type_doc)
        out: list[KeyCandidate] = []
        if kind == "address":
            for label, raw in self.syms.items():
                out.append(KeyCandidate(
                    label, bytes(raw),
                    frozenset(self._address_contexts.get(label) or ())))
            return out
        if kind in ("uint", "int", "bool"):
            numbers = dict(self._numbers)
            # Zero is meaningful for every Solidity key type; 1..3 cover
            # constructor-created sentinel/default domains without depending
            # on a particular contract. They are candidates, not assumptions:
            # absent derived locations are discarded by the readers.
            for number in range(4):
                numbers.setdefault(number, set())
            bits = int(width or 256)
            lo = -(1 << (bits - 1)) if kind == "int" else 0
            hi = (1 << (bits - (1 if kind == "int" else 0))) - 1
            for number in sorted(numbers):
                if lo <= number <= hi and (kind != "bool" or number in (0, 1)):
                    out.append(KeyCandidate(
                        f"#{number}", number,
                        frozenset(numbers[number])))
            return out
        if kind == "bytes":
            size = int(width or 0)
            blobs = set(self._blobs.get(size) or ())
            blobs.add(bytes(size))
            for raw in sorted(blobs):
                address_label = (self.address_label(raw)
                                 if size == 32 and any(raw) else None)
                out.append(KeyCandidate(address_label or "0x" + raw.hex(), raw))
            return out
        if kind in ("string", "dynamic-bytes"):
            for value, contexts in sorted(self._strings.items()):
                raw = value.encode() if kind == "string" else bytes.fromhex(
                    value.removeprefix("0x"))
                out.append(KeyCandidate(value, raw, frozenset(contexts)))
        return out

    def add_runtime(self, type_doc: dict, value: Any) -> None:
        """Add typed values discovered while recursively decoding storage."""
        if type_doc.get("base") and isinstance(value, list):
            base_doc = getattr(self, "types", {}).get(type_doc["base"], {})
            # Readers attach the layout type table before feeding discoveries.
            for item in value:
                self.add_runtime(base_doc, item)
            return
        kind, width = self._kind(type_doc)
        values = value if isinstance(value, list) else [value]
        for item in values:
            if kind in ("uint", "int", "bool") and type(item) is int:
                self._numbers.setdefault(item, set())
            elif kind == "bytes" and isinstance(item, str) and item.startswith("0x"):
                try:
                    raw = bytes.fromhex(item[2:])
                except ValueError:
                    continue
                if len(raw) == int(width or 0):
                    self._blobs.setdefault(len(raw), set()).add(raw)
            elif (kind == "bytes" and isinstance(item, str)
                  and item in self.syms
                  and len(bytes(self.syms[item])) == int(width or 0)):
                raw = bytes(self.syms[item])
                self._blobs.setdefault(int(width or 0), set()).add(raw)
                # Feed the NARROWED word back too. A set of addresses keys its
                # position map by the value it stored, which for an AVM account
                # is the low-20 form — so seeding only the registry bytes finds
                # the set's members and then none of their positions.
                if int(width or 0) == 32:
                    self._blobs[32].add(bytes(12) + raw[-20:])


def evm_key_bytes(candidate: KeyCandidate, type_doc: dict,
                  keccak: Callable[[bytes], bytes]) -> bytes:
    kind, width = KeyEvidence._kind(type_doc)
    if kind == "address":
        return bytes(candidate.value)[-20:].rjust(32, b"\0")
    if kind in ("uint", "bool"):
        return int(candidate.value).to_bytes(32, "big")
    if kind == "int":
        return int(candidate.value).to_bytes(32, "big", signed=True)
    if kind == "bytes":
        return bytes(candidate.value).ljust(32, b"\0")
    if kind in ("string", "dynamic-bytes"):
        return bytes(keccak(bytes(candidate.value)))
    raise ValueError(f"unsupported mapping key type: {type_doc.get('label')}")


def avm_key_bytes(candidate: KeyCandidate, type_doc: dict,
                  sha256: Callable[[bytes], bytes]) -> bytes:
    kind, width = KeyEvidence._kind(type_doc)
    if kind == "address":
        return bytes(candidate.value)
    if kind in ("uint", "bool"):
        size = 8 if int(width or 256) <= 64 else 32
        return int(candidate.value).to_bytes(size, "big")
    if kind == "int":
        size = 8 if int(width or 256) <= 64 else 32
        return int(candidate.value).to_bytes(size, "big", signed=True)
    if kind == "bytes":
        return bytes(candidate.value)
    if kind in ("string", "dynamic-bytes"):
        return bytes(sha256(bytes(candidate.value)))
    raise ValueError(f"unsupported mapping key type: {type_doc.get('label')}")


def avm_key_forms(candidate: KeyCandidate, type_doc: dict,
                  sha256: Callable[[bytes], bytes]) -> list[bytes]:
    """Every byte form this key can take on the AVM leg.

    `address` is the ambiguous one. Under `--contract-abi evm` the contract has
    ONE 160-bit namespace: `msg.sender` lowers to `bzero(12) ++ Sender[12:32]`
    and calldata addresses decode 20 bytes zero-extended. So the low-20 form is
    the only one runtime code can reach, and it is the only one derived here.

    Deliberately NOT also trying the full 32-byte account. A box under that form
    can only have been written by an address that skipped the narrowing — a
    constructor argument delivered over the ARC4 lifecycle path (bgb: the ctor
    mints `_balances[vault]` at the full account while every transfer reads the
    narrowed key, so the mint is orphaned). The EVM leg holds that same entry,
    so accepting the full form makes the pair MATCH and reads as clean state;
    bgb only surfaced because its transfers also reverted. Verified equivalent
    on the corpus: Aave 79/79 `_spokes` and 0 unattributed boxes either way.
    """
    kind, _ = KeyEvidence._kind(type_doc)
    if kind == "address":
        return [bytes(candidate.value)[-20:].rjust(32, b"\0")]
    return [avm_key_bytes(candidate, type_doc, sha256)]


def _path_label(parts: tuple[KeyCandidate, ...]) -> str:
    return "->".join(part.label for part in parts)


class EvmStorageReader:
    """Recursive reader over a solc layout and an EVM slot-word function."""

    def __init__(self, layout: dict, word: Callable[[int], bytes],
                 evidence: KeyEvidence, keccak: Callable[[bytes], bytes],
                 written_slots: set[int] | None = None,
                 max_mapping_paths: int = 75_000,
                 mapping_key_encoder: Callable[[KeyCandidate, dict], bytes] | None = None,
                 full_word_addresses: bool = False):
        self.layout = layout
        self.types = layout.get("types") or {}
        self._word_source = word
        self.evidence = evidence
        self.evidence.types = self.types
        self.keccak = keccak
        self.written_slots = written_slots or set()
        self.max_mapping_paths = max_mapping_paths
        self.mapping_key_encoder = mapping_key_encoder
        self.full_word_addresses = full_word_addresses
        self.seen: dict[int, str] = {}
        self.current_root = ""
        self._paths = 0

    def word(self, slot: int, path: str = "") -> bytes:
        self.seen.setdefault(slot, path or self.current_root)
        return bytes(self._word_source(slot)).rjust(32, b"\0")[-32:]

    @staticmethod
    def _decode(raw: bytes, label: str, fold) -> Any:
        if label == "address" or label.startswith("contract "):
            return fold("0x" + raw[-20:].hex())
        if label == "bool":
            return bool(raw[-1] if raw else 0)
        if label.startswith("uint") or label.startswith("enum "):
            return int.from_bytes(raw, "big")
        if label.startswith("int"):
            return int.from_bytes(raw, "big", signed=True)
        if re.match(r"^bytes\d+$", label):
            return "0x" + raw.hex()
        return "0x" + raw.hex()

    def _scalar(self, slot: int, type_doc: dict, fold, *, offset: int = 0,
                path: str = "") -> tuple[Any, bool]:
        word = self.word(slot, path)
        size = int(type_doc.get("numberOfBytes", 32))
        raw = word[32 - offset - size:32 - offset]
        label = type_doc.get("label", "uint256")
        if (self.full_word_addresses
                and (label == "address" or label.startswith("contract "))
                and any(word)):
            whole = fold("0x" + word.hex())
            if not str(whole).startswith("?"):
                return whole, True
        if label == "bytes32" and any(raw):
            address_label = self.evidence.address_label(raw)
            if address_label is not None:
                return address_label, True
        return self._decode(raw, label, fold), any(raw)

    def _array_length(self, type_doc: dict) -> int | None:
        label = str(type_doc.get("label") or "")
        match = re.search(r"\[(\d+)\]$", label)
        if match:
            return int(match.group(1))
        return None

    def _read_array(self, slot: int, type_doc: dict, fold,
                    path: str) -> tuple[Any, bool]:
        dynamic = type_doc.get("encoding") == "dynamic_array"
        if dynamic:
            length_word = self.word(slot, path + ".length")
            length = int.from_bytes(length_word, "big")
            if length > 4096:
                return f"<{length} elements>", True
            base = int.from_bytes(self.keccak(slot.to_bytes(32, "big")), "big")
        else:
            length = self._array_length(type_doc)
            if length is None:
                return None, False
            base = slot
        element = self.types.get(type_doc.get("base"), {})
        size = int(element.get("numberOfBytes", 32))
        packable = (element.get("encoding") == "inplace"
                    and not element.get("members") and size <= 32)
        values, present = [], bool(length) and not dynamic
        for index in range(length):
            if packable:
                per_word = max(1, 32 // max(1, size))
                element_slot = base + index // per_word
                offset = (index % per_word) * size
                value, hit = self._scalar(
                    element_slot, element, fold, offset=offset,
                    path=f"{path}[{index}]")
            else:
                stride = max(1, (size + 31) // 32)
                value, hit = self.read_value(
                    base + index * stride, type_doc.get("base"), fold,
                    f"{path}[{index}]")
            values.append(value)
            present = present or hit
        self.evidence.add_runtime(element, values)
        return values, present or (dynamic and length > 0)

    def _read_bytes(self, slot: int, type_doc: dict,
                    path: str) -> tuple[Any, bool]:
        word = self.word(slot, path)
        if not any(word):
            return "0x", False
        if word[-1] % 2 == 0:
            length = word[-1] // 2
            raw = word[:length]
        else:
            length = (int.from_bytes(word, "big") - 1) // 2
            base = int.from_bytes(self.keccak(slot.to_bytes(32, "big")), "big")
            raw = b"".join(self.word(base + i, f"{path}[{i}]")
                           for i in range((length + 31) // 32))[:length]
        if type_doc.get("label") == "string":
            return raw.decode("utf-8", "replace"), True
        return "0x" + raw.hex(), True

    def _read_struct(self, slot: int, type_doc: dict, fold,
                     path: str) -> tuple[Any, bool]:
        values, present = [], False
        for member in type_doc.get("members") or []:
            member_type = self.types.get(member.get("type"), {})
            member_slot = slot + int(member.get("slot", 0))
            member_path = f"{path}.{member.get('label')}"
            if (member_type.get("encoding") == "inplace"
                    and not member_type.get("members")
                    and not self._array_length(member_type)):
                value, hit = self._scalar(
                    member_slot, member_type, fold,
                    offset=int(member.get("offset", 0)), path=member_path)
            else:
                value, hit = self.read_value(
                    member_slot, member.get("type"), fold, member_path)
            values.append(value)
            present = present or hit
            self.evidence.add_runtime(member_type, value)
        return values, present

    def _read_mapping(self, slot: int, type_doc: dict, fold, path: str,
                      parts: tuple[KeyCandidate, ...] = ()) -> tuple[dict, bool]:
        key_type = self.types.get(type_doc.get("key"), {})
        value_tid = type_doc.get("value")
        value_type = self.types.get(value_tid, {})
        out: dict[str, Any] = {}
        candidates = self.evidence.candidates(key_type)
        if parts and parts[-1].contexts:
            related = [candidate for candidate in candidates
                       if (not candidate.contexts
                           or candidate.contexts & parts[-1].contexts)]
            if related:
                candidates = related
        for candidate in candidates:
            if self._paths >= self.max_mapping_paths:
                break
            self._paths += 1
            key = (self.mapping_key_encoder(candidate, key_type)
                   if self.mapping_key_encoder
                   else evm_key_bytes(candidate, key_type, self.keccak))
            derived = int.from_bytes(
                self.keccak(key + slot.to_bytes(32, "big")), "big")
            next_parts = parts + (candidate,)
            if value_type.get("encoding") == "mapping":
                nested, hit = self._read_mapping(
                    derived, value_type, fold, path, next_parts)
                if hit:
                    out.update(nested)
                continue
            value, hit = self.read_value(
                derived, value_tid, fold,
                f"{path}[{_path_label(next_parts)}]")
            if hit:
                out[_path_label(next_parts)] = value
        return out, bool(out)

    def read_value(self, slot: int, type_id: str, fold,
                   path: str) -> tuple[Any, bool]:
        type_doc = self.types.get(type_id, {})
        encoding = type_doc.get("encoding")
        if encoding == "mapping":
            return self._read_mapping(slot, type_doc, fold, path)
        if encoding == "dynamic_array" or self._array_length(type_doc) is not None:
            return self._read_array(slot, type_doc, fold, path)
        if encoding == "bytes":
            return self._read_bytes(slot, type_doc, path)
        if encoding == "inplace" and type_doc.get("members"):
            return self._read_struct(slot, type_doc, fold, path)
        return self._scalar(slot, type_doc, fold, path=path)

    def read(self, fold) -> dict:
        scalars, maps, declared = {}, {}, []
        for entry in self.layout.get("storage") or []:
            self.current_root = entry.get("label", "")
            type_doc = self.types.get(entry.get("type"), {})
            encoding = type_doc.get("encoding")
            label = entry.get("label", "")
            slot = int(entry.get("slot", 0))
            if encoding == "mapping":
                declared.append(label)
                self._paths = 0
                value, _ = self._read_mapping(slot, type_doc, fold, label)
                maps[label] = value
                continue
            # Top-level aggregates stay out of the scalar name/value diff, but
            # are still walked so every owned slot is attributed for coverage.
            if (type_doc.get("members") or encoding in ("dynamic_array", "bytes")
                    or self._array_length(type_doc) is not None):
                self.read_value(slot, entry.get("type"), fold, label)
                continue
            value, _ = self._scalar(
                slot, type_doc, fold,
                offset=int(entry.get("offset", 0)), path=label)
            scalars[label] = value
        maps["__declared__"] = sorted(declared)
        return {"scalars": scalars, "maps": maps}


class NativeStorageReader:
    """Recursive ARC-56/native-box twin of :class:`EvmStorageReader`."""

    def __init__(self, layout: dict, arc56: dict, box_values: dict[bytes, bytes],
                 evidence: KeyEvidence, sha256: Callable[[bytes], bytes], fold,
                 max_mapping_paths: int = 75_000):
        self.layout = layout
        self.types = layout.get("types") or {}
        self.arc56 = arc56 or {}
        self.structs = self.arc56.get("structs") or {}
        self.box_values = box_values
        self.evidence = evidence
        self.evidence.types = self.types
        self.sha256 = sha256
        self.fold = fold
        self.max_mapping_paths = max_mapping_paths
        self.matched: set[bytes] = set()
        self._paths = 0

    def _struct_name(self, type_doc: dict) -> str | None:
        label = str(type_doc.get("label") or "")
        if not label.startswith("struct "):
            return None
        return label.split(".")[-1]

    def _arc_type(self, type_id: str) -> str | None:
        type_doc = self.types.get(type_id, {})
        struct_name = self._struct_name(type_doc)
        if struct_name in self.structs:
            fields = self.structs[struct_name]
            members = []
            for field in fields:
                field_type = str(field.get("type") or "")
                members.append(self._resolve_arc_type(field_type))
            return "(" + ",".join(members) + ")"
        label = str(type_doc.get("label") or "")
        if re.match(r"^bytes\d+$", label):
            return "byte[" + label[5:] + "]"
        if label == "bytes":
            return "byte[]"
        if label.startswith("contract "):
            return "address"
        if type_doc.get("encoding") in ("dynamic_array", "inplace") \
                and type_doc.get("base"):
            base = self._arc_type(type_doc["base"])
            if not base:
                return None
            length = self._array_length(type_doc)
            return base + (f"[{length}]" if length is not None else "[]")
        if label.startswith(("uint", "int")) or label in ("address", "bool", "string"):
            return label
        return None

    def _resolve_arc_type(self, typ: str) -> str:
        base = typ.rstrip("[]0123456789")
        suffix = typ[len(base):]
        if base in self.structs:
            body = "(" + ",".join(
                self._resolve_arc_type(str(field.get("type") or ""))
                for field in self.structs[base]) + ")"
            return body + suffix
        match = re.fullmatch(r"bytes(\d+)", base)
        if match:
            return f"byte[{match.group(1)}]" + suffix
        return typ

    @staticmethod
    def _array_length(type_doc: dict) -> int | None:
        match = re.search(r"\[(\d+)\]$", str(type_doc.get("label") or ""))
        return int(match.group(1)) if match else None

    def _canon_arc(self, value: Any, typ: str) -> Any:
        if typ in self.structs:
            return [self._canon_arc(item, field.get("type", ""))
                    for item, field in zip(value or [], self.structs[typ])]
        # ARC-56 spells Solidity fixed bytes as byte[N]. Treat it as one blob,
        # not as an arbitrary array of integers.
        byte_match = re.fullmatch(r"(?:byte|bytes)\[(\d+)\]", typ)
        if byte_match:
            raw = bytes(value or [])
            if len(raw) == 32 and any(raw):
                address_label = self.evidence.address_label(raw)
                if address_label is not None:
                    return address_label
            return "0x" + raw.hex()
        array_match = re.match(r"^(.*)\[(\d*)\]$", typ)
        if array_match:
            return [self._canon_arc(item, array_match.group(1))
                    for item in value or []]
        if typ == "address":
            return self.fold(value)
        if isinstance(value, tuple):
            return [self._canon_arc(item, "") for item in value]
        if isinstance(value, list):
            return [self._canon_arc(item, "") for item in value]
        if isinstance(value, (bytes, bytearray)):
            return "0x" + bytes(value).hex()
        return value

    def _decode_box(self, name: bytes, type_id: str,
                    arc_hint: str | None = None) -> tuple[Any, bool]:
        if name not in self.box_values:
            return None, False
        self.matched.add(name)
        raw = self.box_values[name]
        type_doc = self.types.get(type_id, {})
        label = str(type_doc.get("label") or "")
        arc_type = self._resolve_arc_type(arc_hint) if arc_hint else self._arc_type(type_id)
        if not raw:
            return 0, True
        if arc_type:
            try:
                from algosdk import abi
                decoded = abi.ABIType.from_string(arc_type).decode(raw)
                struct_name = self._struct_name(type_doc)
                return self._canon_arc(decoded, struct_name or arc_hint or arc_type), True
            except Exception:
                pass
        if label == "address" and len(raw) == 32:
            return self.fold(raw), True
        if label == "bytes32" and len(raw) == 32 and any(raw):
            address_label = self.evidence.address_label(raw)
            if address_label is not None:
                return address_label, True
        if label == "bool":
            return bool(int.from_bytes(raw, "big")), True
        if label.startswith("int"):
            bits = int(re.match(r"^int(\d*)", label).group(1) or 256)
            unsigned = int.from_bytes(raw, "big")
            if unsigned >= (1 << (bits - 1)):
                unsigned -= 1 << bits
            return unsigned, True
        if label.startswith(("uint", "enum ")) or arc_hint in ("AVMBytes", "AVMUint64"):
            return int.from_bytes(raw, "big"), True
        return "0x" + raw.hex(), True

    def _contains_mapping(self, type_id: str, seen: set[str] | None = None) -> bool:
        if not type_id or type_id in (seen or set()):
            return False
        seen = set(seen or ()) | {type_id}
        type_doc = self.types.get(type_id, {})
        if type_doc.get("encoding") == "mapping":
            return True
        if type_doc.get("base") and self._contains_mapping(type_doc["base"], seen):
            return True
        return any(self._contains_mapping(member.get("type"), seen)
                   for member in type_doc.get("members") or [])

    def _read_struct(self, prefix: bytes, type_id: str,
                     arc_hint: str | None, path: str) -> tuple[Any, bool]:
        type_doc = self.types.get(type_id, {})
        struct_name = self._struct_name(type_doc)
        decoded, box_hit = self._decode_box(
            prefix, type_id, arc_hint or struct_name)
        members = type_doc.get("members") or []
        values = (list(decoded) if box_hit and isinstance(decoded, (list, tuple))
                  else [None] * len(members))
        if len(values) < len(members):
            values.extend([None] * (len(members) - len(values)))
        present = box_hit
        for index, member in enumerate(members):
            member_tid = member.get("type")
            member_type = self.types.get(member_tid, {})
            if member_type.get("encoding") == "mapping":
                nested, hit = self._read_mapping(
                    prefix + member.get("label", "").encode(), member_type,
                    f"{path}.{member.get('label')}")
                values[index] = nested
                present = present or hit
            elif self._contains_mapping(member_tid):
                nested, hit = self._read_struct(
                    prefix + member.get("label", "").encode(), member_tid,
                    self._struct_name(member_type),
                    f"{path}.{member.get('label')}")
                values[index] = nested
                present = present or hit
            if values[index] is not None:
                self.evidence.add_runtime(member_type, values[index])
        return values, present

    def _read_value(self, prefix: bytes, type_id: str,
                    arc_hint: str | None, path: str) -> tuple[Any, bool]:
        type_doc = self.types.get(type_id, {})
        if type_doc.get("members"):
            return self._read_struct(prefix, type_id, arc_hint, path)
        return self._decode_box(prefix, type_id, arc_hint)

    def _read_mapping(self, prefix: bytes, type_doc: dict, path: str,
                      arc_hint: str | None = None,
                      parts: tuple[KeyCandidate, ...] = ()) -> tuple[dict, bool]:
        key_type = self.types.get(type_doc.get("key"), {})
        value_tid = type_doc.get("value")
        value_type = self.types.get(value_tid, {})
        out: dict[str, Any] = {}
        candidates = self.evidence.candidates(key_type)
        if parts and parts[-1].contexts:
            related = [candidate for candidate in candidates
                       if (not candidate.contexts
                           or candidate.contexts & parts[-1].contexts)]
            if related:
                candidates = related
        for candidate in candidates:
            if self._paths >= self.max_mapping_paths:
                break
            self._paths += 1
            next_parts = parts + (candidate,)
            for encoded in avm_key_forms(candidate, key_type, self.sha256):
                derived = self.sha256(encoded + prefix)
                if value_type.get("encoding") == "mapping":
                    nested, hit = self._read_mapping(
                        derived, value_type, path, arc_hint, next_parts)
                    if hit:
                        out.update(nested)
                        break
                    continue
                value, hit = self._read_value(
                    derived, value_tid, arc_hint,
                    f"{path}[{_path_label(next_parts)}]")
                if hit:
                    out[_path_label(next_parts)] = value
                    break
        return out, bool(out)

    def read_maps(self) -> dict:
        bmaps = (((self.arc56.get("state") or {}).get("maps") or {})
                 .get("box") or {})
        entries = {entry.get("label"): entry
                   for entry in self.layout.get("storage") or []}
        out: dict[str, Any] = {"__declared__": sorted(bmaps)}
        unsupported = []
        for name, spec in bmaps.items():
            entry = entries.get(name)
            if not entry:
                unsupported.append(name)
                continue
            type_doc = self.types.get(entry.get("type"), {})
            if type_doc.get("encoding") != "mapping":
                unsupported.append(name)
                continue
            self._paths = 0
            value, _ = self._read_mapping(
                name.encode(), type_doc, name, spec.get("valueType"))
            out[name] = value
        if unsupported:
            out["__unsupported__"] = sorted(unsupported)
        return out


def grouped_uncovered_slots(slots: Iterable[int], writes_by_slot: dict[int, list[int]],
                            calls: list[dict]) -> dict:
    """Group genuinely unresolved writes by physical class and call signature."""
    groups: dict[str, dict] = {}
    for slot in sorted(slots):
        group = "namespaced_or_assembly" if slot >= (1 << 128) else "unresolved_layout"
        item = groups.setdefault(group, {"slots": 0, "transactions": set(),
                                         "signatures": {}, "sample": []})
        item["slots"] += 1
        txns = writes_by_slot.get(slot) or []
        item["transactions"].update(txns)
        for txn in txns:
            sig = ((calls[txn].get("sig") if 0 <= txn < len(calls) else None)
                   or "<deployment/internal>")
            item["signatures"][sig] = item["signatures"].get(sig, 0) + 1
        if len(item["sample"]) < 12:
            item["sample"].append(str(slot))
    for item in groups.values():
        item["transactions"] = sorted(item["transactions"])
        item["signatures"] = dict(sorted(
            item["signatures"].items(), key=lambda pair: (-pair[1], pair[0])))
    return groups


def build_parameterized_getter_probes(abi: list[dict], calls: list[dict],
                                      fns: dict, max_per_method: int = 32,
                                      max_total: int = 4096) -> list[dict]:
    """Build ABI-driven read probes from argument tuples evidenced by replay.

    No function or contract names are recognised. Unary getters use the global
    pool for their exact ABI type. Multi-argument getters use tuples observed
    together in a transaction context, avoiding an unbounded global Cartesian
    product while preserving relationships such as ``(assetId, spoke)``.
    """
    contexts: list[tuple[int, dict[str, list[Any]]]] = []
    pools: dict[str, list[Any]] = {}
    pool_sources: dict[tuple[str, str], int] = {}

    def add(dst: dict[str, list[Any]], typ: str, value: Any) -> None:
        vals = dst.setdefault(typ, [])
        key = _marker_key(value)
        if all(_marker_key(old) != key for old in vals):
            vals.append(value)

    def collect(dst: dict[str, list[Any]], value: Any, spec: dict) -> None:
        add(dst, canonical_abi_type(spec), value)
        elem = _array_element_spec(spec)
        if elem is not None:
            for item in value or []:
                collect(dst, item, elem)
        elif spec.get("type") == "tuple":
            for item, component in zip(value or [], spec.get("components") or []):
                collect(dst, item, component)

    for call_index, call in enumerate(calls or []):
        source = int(call.get("i", call_index))
        local: dict[str, list[Any]] = {}
        sender = call.get("sender")
        if isinstance(sender, dict) and set(sender) == {"__addr__"}:
            add(local, "address", sender)
        inputs = ((fns.get(call.get("sig")) or {}).get("inputs") or [])
        for value, spec in zip(call.get("args") or [], inputs):
            collect(local, value, spec)
        for typ, values in local.items():
            for value in values:
                add(pools, typ, value)
                pool_sources.setdefault((typ, _marker_key(value)), source)
        contexts.append((source, local))

    probes: list[dict] = []
    for entry in abi or []:
        inputs = entry.get("inputs") or []
        if (entry.get("type") != "function" or not inputs
                or not entry.get("outputs")
                or entry.get("stateMutability") not in ("view", "pure")):
            continue
        sig = entry["name"] + "(" + ",".join(
            canonical_abi_type(spec) for spec in inputs) + ")"
        types = [canonical_abi_type(spec) for spec in inputs]
        tuples: list[list[Any]] = []
        tuple_sources: dict[str, int] = {}

        def add_tuple(items: Iterable[Any], source: int | None = None) -> None:
            value = list(items)
            key = _marker_key(value)
            if len(tuples) < max_per_method and all(
                    _marker_key(old) != key for old in tuples):
                tuples.append(value)
                if source is not None:
                    tuple_sources[key] = source

        # Exact historical invocations are always the strongest evidence.
        for call in calls or []:
            if call.get("sig") == sig:
                add_tuple(call.get("args") or [], int(call.get("i", 0)))
        if len(types) == 1:
            values = list(pools.get(types[0], ()))
            if re.fullmatch(r"u?int\d*", types[0]):
                values.sort(key=lambda value: (
                    not (type(value) is int and -4096 <= value <= 4096),
                    abs(value) if type(value) is int else 0))
            for value in values:
                add_tuple([value], pool_sources.get((types[0], _marker_key(value))))
        else:
            for source, local in contexts:
                choices = [local.get(typ) or [] for typ in types]
                if all(choices):
                    for combination in itertools.product(*choices):
                        add_tuple(combination, source)
                        if len(tuples) >= max_per_method:
                            break
                if len(tuples) >= max_per_method:
                    break
        for args in tuples:
            probe = {"sig": sig, "args": args,
                     "outputs": entry.get("outputs") or []}
            source = tuple_sources.get(_marker_key(args))
            if source is not None:
                probe["source_txn"] = source
            probes.append(probe)
            if len(probes) >= max_total:
                return probes
    return probes
