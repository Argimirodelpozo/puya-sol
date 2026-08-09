"""Value helpers for Solidity isoltest semantics.

Solidity's semantic test format expresses padded values explicitly:
    left(0x4200ef)   → 0x4200ef00...00  (right-padded to 32 bytes)
    right(0x4200ef)  → 0x00...004200ef  (left-padded to 32 bytes)

In test functions written against the new framework, the assertion side
holds typed Python values. These helpers exist for the rare case where
a test really does want a specific 32-byte layout (typically `bytes32`
state vars whose raw bytes layout matters).
"""
from __future__ import annotations

# Set by call.Result whenever a call reverts; as_int uses it to explain a None
# return instead of raising an opaque TypeError. Per-process, tests are serial.
_LAST_REVERT = ""


def lpad(value: bytes | int, width: int = 32) -> bytes:
    """Left-pad value to `width` bytes (typical Solidity uint encoding)."""
    if isinstance(value, int):
        return value.to_bytes(width, "big", signed=False)
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"lpad expects bytes or int, got {type(value).__name__}")
    if len(value) > width:
        raise ValueError(f"value too long: {len(value)} > {width}")
    return b"\x00" * (width - len(value)) + bytes(value)


def rpad(value: bytes | str, width: int = 32) -> bytes:
    """Right-pad value to `width` bytes (typical Solidity bytesN/string encoding)."""
    if isinstance(value, str):
        value = value.encode()
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"rpad expects bytes or str, got {type(value).__name__}")
    if len(value) > width:
        raise ValueError(f"value too long: {len(value)} > {width}")
    return bytes(value) + b"\x00" * (width - len(value))


def u256(value: int) -> int:
    """Wrap value to unsigned 256-bit (mod 2**256)."""
    return value & ((1 << 256) - 1)


def i256(value: int) -> int:
    """Encode signed Python int as two's-complement uint256."""
    if value >= 0:
        return value & ((1 << 256) - 1)
    return ((1 << 256) + value) & ((1 << 256) - 1)


def hex_bytes(s: str) -> bytes:
    """`hex_bytes("0x4200ef")` or `hex_bytes("4200ef")` → bytes."""
    if s.startswith("0x"):
        s = s[2:]
    if len(s) % 2 == 1:
        s += "0"  # right-pad odd-length hex like Solidity does
    return bytes.fromhex(s)


def str_bytes(s: str) -> bytes:
    """ASCII encode a Python string."""
    return s.encode("ascii")


def raw(value: bytes) -> bytes:
    """Identity marker. Used in test code to signal 'this is the raw byte sequence
    the contract returns; do not interpret it as ABI-decoded'."""
    return value


def as_signed_int(value, bits: int = 256) -> int:
    """Coerce algosdk's typed return into a signed Python int.

    Use when a Solidity function declared `returns (int<N>)` (or any
    signed integer type) but our ABI surfacing emits the value as a
    32-byte uint256 (two's-complement payload). algosdk decodes that
    as a positive biguint; this helper reinterprets the top `bits`
    bits as signed two's complement.

    Examples:
        as_signed_int(2**256 - 5)        → -5     (default 256 bits)
        as_signed_int(0xfb, bits=8)      → -5
        as_signed_int(42)                →  42

    When the value is already a negative Python int (e.g. algokit's
    patched SignedIntType returned `int8` natively), it is passed
    through unchanged.
    """
    if isinstance(value, int) and not isinstance(value, bool):
        if value < 0:
            return value
        n = value
    else:
        n = as_int(value)
    sign_bit = 1 << (bits - 1)
    if n & sign_bit:
        n -= 1 << bits
    return n


def as_int(value) -> int:
    """Coerce algosdk's typed return into the equivalent uint256 int.

    Use when a Solidity function declared `returns (bytes32)` (or any
    byte[N]) but the test expectation is written as an int. algosdk
    delivers byte[N] as `list[int]`, so a strict `== 0x...` check fails
    against `[0,0,...]`.

    Conversions:
      int          → unchanged
      bool         → 0 / 1
      bytes        → big-endian int.from_bytes
      list/tuple   → bytes(...) then int.from_bytes
      str (algo)   → decode_address → 32-byte int (last 20 bytes match an EVM address)
    """
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, (bytes, bytearray)):
        return int.from_bytes(value, "big")
    if isinstance(value, (list, tuple)):
        # Filter out non-int items (defensive) — algosdk delivers byte[N] as
        # a list[int] in [0, 255].
        if all(isinstance(x, int) for x in value):
            return int.from_bytes(bytes(value), "big")
    if isinstance(value, str):
        # Algorand address (58-char base32). decode_address yields 32 bytes.
        from algosdk import encoding as _enc
        try:
            return int.from_bytes(_enc.decode_address(value), "big")
        except Exception:
            pass
    if value is None and _LAST_REVERT:
        raise AssertionError(
            "call returned no value because it REVERTED: " + _LAST_REVERT[:200]
        )
    raise TypeError(f"can't coerce {type(value).__name__} to int: {value!r}")


def as_bytes(value) -> bytes:
    """Coerce algosdk's typed return into a bytes object.

    int → 32-byte big-endian. bytes → unchanged. list[int]/tuple[int] →
    bytes(...). str → utf-8 encode (for `string` returns).
    """
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, (list, tuple)):
        return bytes(value)
    if isinstance(value, int):
        return value.to_bytes(32, "big")
    if isinstance(value, str):
        return value.encode("utf-8")
    raise TypeError(f"can't coerce {type(value).__name__} to bytes: {value!r}")


def evm_words(data) -> tuple[int, ...]:
    """EVM return-data word view of a `bytes`-returning method.

    isoltest expectations like `s() -> 0x20, len, ...` treat the raw EVM
    return blob — outer (offset, length) framing plus the payload padded to
    32-byte words — as a tuple of words. The AVM side returns just the
    payload (ARC4 byte[]); reconstruct the EVM view for those assertions.
    """
    b = bytes(data) if not isinstance(data, (bytes, bytearray)) else bytes(data)
    words = [0x20, len(b)]
    padded = b + b"\x00" * ((32 - len(b) % 32) % 32)
    for i in range(0, len(padded), 32):
        words.append(int.from_bytes(padded[i:i + 32], "big"))
    return tuple(words)


def arc4_selector(sig: str) -> bytes:
    """4-byte ARC-4 method selector: sha512_256(signature)[:4]. puya-sol's
    convention for routers, f.selector, abi.encodeWithSignature/encodeCall,
    events and custom errors (EVM uses keccak256 — EVM_DIVERGENCE)."""
    import hashlib
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def arc4_event_topic(sig: str) -> int:
    """Event .selector value on AVM: the FULL 32-byte sha512_256(signature)
    as an int (EVM uses keccak256 topic0 — EVM_DIVERGENCE). Its first 4
    bytes equal the ARC-28 log prefix puya-sol emits."""
    import hashlib
    return int.from_bytes(hashlib.new("sha512_256", sig.encode()).digest(), "big")


def arc4_encode(type_str: str, value) -> bytes:
    """ARC-4 encoding of `value` as the ARC-4 ABI type `type_str`, the oracle
    for what puya-sol's abi.encode/abi.decode produce/consume now that the
    internal encoding is ARC4 everywhere (EVM_DIVERGENCE: on real EVM these
    would be the EVM ABI head/tail layout). Mirror of how `eth_abi` was used as
    the EVM oracle before the ARC4 migration. Tuple example:
    arc4_encode("(uint256,string)", [1, "abc"]).

    NB ARC-4 type spelling differs from EVM in places: dynamic bytes is
    `byte[]`, a fixed bytesN is `byte[N]`, address is `address` (a 32-byte
    account on AVM, not 20)."""
    from algosdk.abi import ABIType
    return ABIType.from_string(type_str).encode(value)
