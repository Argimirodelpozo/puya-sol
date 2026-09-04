"""Storage-semantics parity matrix vs solc (solc-equivalence audit).

StorageParitySlots (slot mode only): raw sload() asserts against the words
solc's rules pin — packed byte positions from the LSB, mapping slot =
keccak(encodedKey ++ slot32) with RAW string keys and sign-extended signed
keys, dynamic data at keccak(slot) with byte-packed elements, pop() zeroing
the vacated byte, short/long string forms.

StorageParityCore (both modes): delete-struct keeps mappings, delete-array
zeroes elements, push() reference + zero default, pop-then-push reads zero.
"""
import pytest

SOURCE = "puyasolRegression/contracts/storage_parity_matrix.sol"

SLOT_PROBES = [
    "packedWord()", "packedWrite()", "stringKey()", "signedKey()",
    "packedArray()", "popZeroes()", "shortString()", "longString()",
    "signedArrayWord()", "shortLongRoundTrip()", "slotOffsetFacts()",
]
CORE_PROBES = [
    "deleteStructKeepsMapping()", "deleteArrayZeroesElements()",
    "pushRefAndDefault()", "popThenPushReadsZero()",
    "structStorageCopy()", "arrayStorageCopy()",
]


def _run(harness, app, sig):
    r = harness.call(app, sig, extra_fee=20_000)
    assert not r.reverted, f"{sig}: {r.fail_message}"
    ret = r.abi_return
    ok = ret[0] if isinstance(ret, (list, tuple)) else ret
    assert ok is True, f"{sig} parity failed: {ret}"


def test_slot_mode_raw_word_parity(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--evm-storage-layout"])
    app = harness.deploy(artifacts, "StorageParitySlots")
    for sig in SLOT_PROBES:
        _run(harness, app, sig)


@pytest.mark.parametrize("mode", ["default", "slot"])
def test_core_semantics_both_modes(harness, mode):
    extra = ["--evm-storage-layout"] if mode == "slot" else []
    artifacts = harness.compile(SOURCE, extra_args=extra)
    app = harness.deploy(artifacts, "StorageParityCore")
    for sig in CORE_PROBES:
        _run(harness, app, sig)


BOOLS = "puyasolRegression/contracts/storage_parity_bools.sol"


def test_bool_array_slot_mode_parity(harness):
    """Slot mode: byte-consistent bool[] incl. the T,T discriminator and
    raw-word + pop-zeroing checks (oracle-endorsed expectations)."""
    artifacts = harness.compile(BOOLS, extra_args=["--evm-storage-layout"])
    app = harness.deploy(artifacts, "BoolArrayParity")
    for sig in ("pushReadTrueTrue()", "wordAndOps()"):
        _run(harness, app, sig)


def test_bool_array_default_mode_fails_loud(harness):
    """Default mode: storage bool[] is puyabug.md #10 (silent wrong reads) —
    must be a COMPILE error pointing at --evm-storage-layout."""
    from framework.compile import CompileError
    with pytest.raises(CompileError, match="bool"):
        harness.compile(BOOLS)
