"""Multi-slot memory audit — "who still assumes slot 0" (new_review.md B7/B8).

EVM memory is modeled as N scratch slots of 4096 bytes each. The multi-slot
migration reached the range helpers but not the runtime-offset word paths:

B7: readMemWordDirect/writeMemWordDirect never stitched a word straddling a
    4096-byte boundary (their constant-offset twins do), so extract3/replace3
    ran off the end of the slot and panicked on code EVM runs fine. The range
    word-loops (mcopy, returndatacopy, precompile I/O) reach them from
    arbitrary unaligned offsets.
B8: statement-position memory writers (mstore8, mcopy, calldatacopy,
    returndatacopy, call output copies) never dropped the mem_0x<off> content
    constants the expression path invalidates, so a later constant-folded
    keccak256 hashed a STALE word — and `sstore(keccak256(...), v)` then wrote
    the wrong storage slot, silently.
"""

from eth_utils import keccak

from framework import as_int

SLOT = 4096
# 32-byte window at 4090 crosses into the next slot (4090 + 32 > 4096).
STRADDLE = SLOT - 6
# The last window that still fits inside one slot — the boundary case.
FITS = SLOT - 32


def _word(v: int) -> bytes:
    return v.to_bytes(32, "big")


def test_word_straddles_slot_boundary(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/memory_slot_audit.sol")

    # Round-trip at a straddling offset, at the exact fits-in-slot boundary,
    # and at the second boundary (slots 1/2) — pre-fix the straddling ones
    # panicked.
    for p in (FITS, STRADDLE, SLOT - 1, 2 * SLOT - 7):
        v = 0xDEADBEEF00000000000000000000000000000000000000000000000000000001
        got = as_int(harness.call(app, "wordRoundTrip(uint256,uint256)", p, v).abi_return)
        assert got == v, f"round-trip failed at offset {p}"

    # A straddling write must not bleed into the neighbouring words.
    r = harness.call(
        app, "straddleNeighbours(uint256,uint256)", STRADDLE, 0x2222).abi_return
    before, mid, after = (as_int(x) for x in r)
    assert (before, mid, after) == (0x1111, 0x2222, 0x3333)


def test_range_copy_across_slot_boundary(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/memory_slot_audit.sol")

    # Runtime length → the readMemRangeDyn/writeMemRangeDyn word loops, whose
    # per-word offsets straddle the boundary.
    r = harness.call(
        app, "copyAcrossBoundary(uint256,uint256,uint256)",
        STRADDLE, STRADDLE + 3 * SLOT, 64).abi_return
    a, b = (as_int(x) for x in r)
    assert (a, b) == (0xAAAA, 0xBBBB)


def test_mem_constants_invalidated_by_statement_writers(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/memory_slot_audit.sol")

    # mstore8 sets the word's TOP byte, so the second hash must differ.
    h1, h2 = harness.call(app, "keccakAfterMstore8()").abi_return
    h1, h2 = bytes(h1), bytes(h2)
    assert h1 == keccak(_word(1))
    assert h2 != h1, "keccak folded over the stale pre-mstore8 word"
    assert h2 == keccak(bytes([0xFF]) + _word(1)[1:])

    # Same gap through a statement-position mcopy.
    h1, h2 = harness.call(app, "keccakAfterMcopy()").abi_return
    h1, h2 = bytes(h1), bytes(h2)
    assert h1 == keccak(_word(1))
    assert h2 == keccak(_word(2)), "keccak folded over the stale pre-mcopy word"
