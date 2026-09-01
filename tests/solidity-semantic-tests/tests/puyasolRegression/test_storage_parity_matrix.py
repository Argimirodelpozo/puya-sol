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
    "boolArrayWord()", "signedArrayWord()", "shortLongRoundTrip()",
    "slotOffsetFacts()",
]
CORE_PROBES = [
    "deleteStructKeepsMapping()", "deleteArrayZeroesElements()",
    "pushRefAndDefault()", "popThenPushReadsZero()",
    "structStorageCopy()", "arrayStorageCopy()", "boolArrayCore()",
]


def _run(harness, app, sig):
    r = harness.call(app, sig, extra_fee=20_000)
    assert not r.reverted, f"{sig}: {r.fail_message}"
    ret = r.abi_return
    ok = ret[0] if isinstance(ret, (list, tuple)) else ret
    assert ok is True, f"{sig} parity failed: {ret}"


def test_slot_mode_raw_word_parity(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--evm-layout"])
    app = harness.deploy(artifacts, "StorageParitySlots")
    for sig in SLOT_PROBES:
        _run(harness, app, sig)


@pytest.mark.parametrize("mode", ["default", "slot"])
def test_core_semantics_both_modes(harness, mode):
    extra = ["--evm-layout"] if mode == "slot" else []
    artifacts = harness.compile(SOURCE, extra_args=extra)
    app = harness.deploy(artifacts, "StorageParityCore")
    for sig in CORE_PROBES:
        _run(harness, app, sig)
