#!/usr/bin/env python3
"""Compare EVM vs AVM revert PAYLOADS in the stateful differential.

The oracle already diffs revert STATUS; this adds the payload. Both sides are
normalised to a canonical descriptor and compared with tolerances for the
divergences that are documented + by-design:

  kind        EVM                         AVM                       compared?
  --------    -------------------------   -----------------------   -----------------
  empty       revert()/require(no msg)    same                      yes (both empty)
  Error       0x08c379a0 + string         0x08c379a0 + string       yes — message EXACT
  Panic       0x4e487b71 + code           (AVM emits NO payload)     tolerated (Panic
                                                                     rejected on AVM —
                                                                     status already matched)
  custom      keccak sel + ABI-width args sha512_256 sel + backing   kind only (selectors
                                          -width args                differ by design;
                                                                     arg VALUES covered by
                                                                     fuzz_revert.py)

So this catches: a revert emitting the WRONG KIND (Error vs custom vs empty), and a
corrupted Error(string) MESSAGE — without false-flagging the keccak/sha512_256 and
backing-width conventions.
"""

_ERROR_SEL = "08c379a0"   # keccak("Error(string)")[:4] — EVM-literal magic on both sides
_PANIC_SEL = "4e487b71"   # keccak("Panic(uint256)")[:4]


def _as_bytes(payload):
    if payload is None:
        return None
    if isinstance(payload, (bytes, bytearray)):
        return bytes(payload)
    s = payload[2:] if payload[:2] in ("0x", "0X") else payload
    try:
        return bytes.fromhex(s)
    except ValueError:
        return None


def _decode_string(data):
    """ABI-decode a single `string` from an Error(string) tail (offset+len+utf8)."""
    try:
        if len(data) < 64:
            return None
        length = int.from_bytes(data[32:64], "big")
        return data[64:64 + length].decode("utf-8", "replace")
    except Exception:
        return None


def canon_revert(payload):
    """Raw revert payload (hex str or bytes) → canonical descriptor tuple.
    None → ('unknown',) (couldn't obtain payload — don't compare)."""
    b = _as_bytes(payload)
    if b is None:
        return ("unknown",)
    if len(b) == 0:
        return ("empty",)
    sel = b[:4].hex()
    if sel == _ERROR_SEL:
        return ("Error", _decode_string(b[4:]))
    if sel == _PANIC_SEL:
        return ("Panic",)                 # code intentionally ignored (see module doc)
    return ("custom",)                    # kind only — selector/args diverge by design


def revert_match(evm_payload, avm_payload):
    """(ok, evm_desc, avm_desc). Applies the documented-divergence tolerances."""
    ev = canon_revert(evm_payload)
    av = canon_revert(avm_payload)
    if "unknown" in (ev[0], av[0]):
        return True, ev, av               # payload unavailable on a side — skip
    # Panic is rejected on AVM (emits no payload); status already matched → tolerate.
    if ev[0] == "Panic":
        return True, ev, av
    return ev == av, ev, av
