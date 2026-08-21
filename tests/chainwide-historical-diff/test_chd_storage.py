"""Guards for the two harness faults Aave's replay exposed.

Both produced findings that looked like compiler divergences and were not:

* an AVM account narrowed by a Solidity `address` was keyed and rendered at its
  full 32 bytes, so 45 of Aave's 79 `_spokes` entries were never found and the
  ones that were showed raw hex against the EVM leg's symbols;
* the post-replay probe phase was pinned on neither leg, so every accruing view
  drifted by a uniform fraction of the window.
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from chd_common import probe_clock_target, symbol  # noqa: E402
from chd_storage import (KeyCandidate, KeyEvidence, avm_key_forms,  # noqa: E402
                         evm_key_bytes)

ADDRESS = {"label": "address", "numberOfBytes": "20"}

# A real deterministic sender account (sha256("chd-algo-sender-1") -> ed25519
# pubkey). The leading 12 bytes are exactly what a Solidity `address` drops —
# mistaking the surviving 20 for a mainnet address is what made this look like
# a value divergence rather than a key-form one.
ACCOUNT = (bytes.fromhex("baeed519e049e1d89eccb336")
           + bytes.fromhex("6951f789db4c6aea442069372796240e9632051a"))
NARROWED = bytes(12) + ACCOUNT[-20:]
# An arg symbol's registry bytes are already 12 zeros + 20.
ARG = bytes(12) + b"\xcd" + bytes(15) + (10002).to_bytes(4, "big")


def _evidence(syms):
    return KeyEvidence([], {}, syms)


def test_avm_address_key_is_the_narrowed_word_only():
    """One namespace, and NOT the full account.

    A box under the full-account form can only come from an address that
    skipped the narrowing — orphaned constructor state the runtime cannot
    reach. The EVM leg holds the same entry, so deriving that form too would
    make the pair match and read as clean.
    """
    forms = avm_key_forms(KeyCandidate("«1»", ACCOUNT), ADDRESS,
                          lambda b: hashlib.sha256(b).digest())
    assert forms == [NARROWED]
    assert ACCOUNT not in forms


def test_the_avm_address_key_equals_the_evm_key():
    sha = lambda b: hashlib.sha256(b).digest()  # noqa: E731
    candidate = KeyCandidate("«1»", ACCOUNT)
    assert (avm_key_forms(candidate, ADDRESS, sha)
            == [evm_key_bytes(candidate, ADDRESS, sha)])


def test_arg_symbol_key_is_unchanged():
    # Args already carry the narrowed form, so nothing moves for them.
    forms = avm_key_forms(KeyCandidate("«10002»", ARG), ADDRESS,
                          lambda b: hashlib.sha256(b).digest())
    assert forms == [ARG]


def test_address_label_matches_the_narrowed_account():
    ev = _evidence({symbol(1): ACCOUNT})
    assert ev.address_label(NARROWED) == symbol(1)
    assert ev.address_label(ACCOUNT) == symbol(1)


def test_address_label_rejects_an_unrelated_word():
    ev = _evidence({symbol(1): ACCOUNT})
    assert ev.address_label(bytes(12) + bytes(range(20))) is None
    # A full-width blob that merely shares the low 20 bytes is NOT the account:
    # narrowing only applies to a word whose top 12 bytes were zeroed.
    assert ev.address_label(b"\x01" * 12 + ACCOUNT[-20:]) is None


def test_reader_resolves_the_narrowed_box_and_ignores_the_orphan():
    """The narrowed box is read; a full-account box is left UNATTRIBUTED.

    Leaving it unattributed is the point — the report then shows a box the
    reader could not place, which is the signal that something wrote outside
    the contract's own address namespace.
    """
    from algosdk import abi
    from chd_storage import NativeStorageReader

    types = {
        "addr": {"encoding": "inplace", "label": "address",
                 "numberOfBytes": "20"},
        "one": {"encoding": "inplace", "label": "struct T.One",
                "numberOfBytes": "32", "members": [
                    {"label": "n", "slot": "0", "offset": 0, "type": "num"}]},
        "num": {"encoding": "inplace", "label": "uint256",
                "numberOfBytes": "32"},
        "map": {"encoding": "mapping", "label": "mapping(address => struct T.One)",
                "numberOfBytes": "32", "key": "addr", "value": "one"},
    }
    layout = {"types": types, "storage": [
        {"label": "_m", "slot": "0", "offset": 0, "type": "map"}]}
    arc56 = {"structs": {"One": [{"name": "n", "type": "uint256"}]},
             "state": {"maps": {"box": {
                 "_m": {"keyType": "AVMBytes", "valueType": "One"}}}}}

    sha = lambda data: hashlib.sha256(data).digest()  # noqa: E731
    encode = abi.ABIType.from_string("(uint256)").encode
    narrowed_box = sha(NARROWED + b"_m")
    orphan_box = sha(ACCOUNT + b"_m")
    boxes = {narrowed_box: encode([22]), orphan_box: encode([11])}
    label = symbol(1)
    reader = NativeStorageReader(
        layout, arc56, boxes, _evidence({label: ACCOUNT}), sha,
        lambda raw: label if bytes(raw) in (ACCOUNT, NARROWED) else "?")

    assert reader.read_maps()["_m"] == {label: [22]}
    assert reader.matched == {narrowed_box}


def test_probe_clock_target_is_one_past_the_last_entry():
    assert probe_clock_target({0: 100, 1: 105, 2: 104}) == 106


def test_probe_clock_target_is_zero_without_a_schedule():
    assert probe_clock_target({}) == 0
