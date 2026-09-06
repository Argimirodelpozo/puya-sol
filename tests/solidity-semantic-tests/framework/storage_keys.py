"""Default holder format 2; inputs are solc layout facts, not source names.

This is not EVM slot storage or ARC-56's prefix-plus-encoded-key map format.
Mapping keys must first use the compiler's declared-key encoding (dynamic
string/bytes keys are SHA-256 digests). See docs/storage-format.md.
"""

from base64 import b85encode
from hashlib import sha256


def _coordinate(slot: int, offset: int) -> bytes:
    if not 0 <= offset < 32:
        raise ValueError("invalid solc byte offset")
    return slot.to_bytes(32, "big") + bytes([offset])


def holder_root(slot: int, offset: int = 0) -> bytes:
    return b"@puya-sol/2:" + b85encode(_coordinate(slot, offset))


def _segment(tag: bytes, parent: bytes, payload: bytes) -> bytes:
    return sha256(b"puya-sol/2/" + tag + len(parent).to_bytes(8, "big") + parent + payload).digest()


def holder_member(parent: bytes, slot: int, offset: int = 0) -> bytes:
    return _segment(b"s", parent, _coordinate(slot, offset))


def holder_array_element(parent: bytes, index: int) -> bytes:
    return _segment(b"a", parent, index.to_bytes(32, "big"))


def mapping_entry(parent: bytes, encoded_key: bytes) -> bytes:
    return _segment(b"m", parent, encoded_key)
