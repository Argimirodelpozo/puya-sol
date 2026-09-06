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


# ── holder format 2 (rev-2 default storage keys, docs/storage-format.md) ───

def test_holder_derivation_matches_the_framework_helper():
    """One derivation, two copies: the reader's and the semantic framework's."""
    import importlib
    from chd_storage import (holder_array_element, holder_mapping_entry,
                             holder_member, holder_root)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]
                           / "solidity-semantic-tests"))
    try:
        keys = importlib.import_module("framework.storage_keys")
    except Exception:  # the EVM venv has no algosdk for framework/__init__
        import pytest
        pytest.skip("framework.storage_keys not importable here")
    root = holder_root(5, 0)
    assert root == keys.holder_root(5, 0) and len(root) == 54
    assert holder_root(1 << 200) == keys.holder_root(1 << 200)
    member = holder_member(root, 2, 16)
    assert member == keys.holder_member(root, 2, 16)
    element = holder_array_element(member, 3)
    assert element == keys.holder_array_element(member, 3)
    assert holder_mapping_entry(element, b"k") == keys.mapping_entry(element, b"k")


def _format2_fixture():
    """A layout with every format-2 shape and boxes built the way rev-2 lays
    them out: mapping roots, a struct-valued map whose struct holds a mapping,
    a transparent wrapper (EnumerableSet.AddressSet) inside a map, and a fixed
    array of mapping-carrying structs."""
    import base64
    from algosdk import abi
    from chd_storage import (holder_array_element, holder_mapping_entry,
                             holder_member, holder_root)
    types = {
        "addr": {"encoding": "inplace", "label": "address", "numberOfBytes": "20"},
        "u256": {"encoding": "inplace", "label": "uint256", "numberOfBytes": "32"},
        "u64": {"encoding": "inplace", "label": "uint64", "numberOfBytes": "8"},
        "b32": {"encoding": "inplace", "label": "bytes32", "numberOfBytes": "32"},
        "b32arr": {"encoding": "dynamic_array", "label": "bytes32[]",
                   "numberOfBytes": "32", "base": "b32"},
        "bal": {"encoding": "mapping", "label": "mapping(address => uint256)",
                "numberOfBytes": "32", "key": "addr", "value": "u256"},
        "allow": {"encoding": "mapping",
                  "label": "mapping(address => mapping(address => uint256))",
                  "numberOfBytes": "32", "key": "addr", "value": "bal"},
        "acct": {"encoding": "inplace", "label": "struct T.Account",
                 "numberOfBytes": "64", "members": [
                     {"label": "tag", "slot": "0", "offset": 0, "type": "u64"},
                     {"label": "sub", "slot": "1", "offset": 0, "type": "bal"}]},
        "accts": {"encoding": "mapping", "label": "mapping(address => struct T.Account)",
                  "numberOfBytes": "32", "key": "addr", "value": "acct"},
        "posmap": {"encoding": "mapping", "label": "mapping(bytes32 => uint256)",
                   "numberOfBytes": "32", "key": "b32", "value": "u256"},
        "set": {"encoding": "inplace", "label": "struct EnumerableSet.Set",
                "numberOfBytes": "64", "members": [
                    {"label": "_values", "slot": "0", "offset": 0, "type": "b32arr"},
                    {"label": "_positions", "slot": "1", "offset": 0, "type": "posmap"}]},
        "aset": {"encoding": "inplace", "label": "struct EnumerableSet.AddressSet",
                 "numberOfBytes": "64", "members": [
                     {"label": "_inner", "slot": "0", "offset": 0, "type": "set"}]},
        "members": {"encoding": "mapping",
                    "label": "mapping(uint256 => struct EnumerableSet.AddressSet)",
                    "numberOfBytes": "32", "key": "u256", "value": "aset"},
        "rows": {"encoding": "inplace", "label": "struct T.Account[2]",
                 "numberOfBytes": "128", "base": "acct"},
    }
    layout = {"types": types, "storage": [
        {"label": "bal", "slot": "0", "offset": 0, "type": "bal"},
        {"label": "allow", "slot": "1", "offset": 0, "type": "allow"},
        {"label": "accts", "slot": "2", "offset": 0, "type": "accts"},
        {"label": "members", "slot": "3", "offset": 0, "type": "members"},
        {"label": "rows", "slot": "4", "offset": 0, "type": "rows"},
        {"label": "total", "slot": "8", "offset": 0, "type": "u256"},
    ]}
    roots = {name: holder_root(slot) for name, slot in
             (("bal", 0), ("allow", 1), ("accts", 2), ("members", 3), ("rows", 4))}
    arc56 = {
        # The compiler names the wrapper but lists the INNER fields under it,
        # with the mapping member as an empty byte[] placeholder.
        "structs": {"AddressSet": [{"name": "_values", "type": "byte[32][]"},
                                   {"name": "_positions", "type": "byte[]"}],
                    "tuple": [{"name": "tag", "type": "uint64"}]},
        "state": {"maps": {"box": {}},
                  "keys": {"box": {
                      name: {"keyType": "AVMString", "valueType": "AVMBytes",
                             "key": base64.b64encode(root).decode(),
                             "desc": "puya-sol holder format 2"}
                      for name, root in roots.items()}}}}
    a1 = NARROWED
    a2 = ARG
    b32_a2 = a2  # bytes32(uint256(uint160(addr))) == the narrowed word
    enc = lambda typ, value: abi.ABIType.from_string(typ).encode(value)  # noqa: E731
    boxes = {root: b"\0\0" for root in roots.values()}
    boxes[holder_mapping_entry(roots["bal"], a1)] = b"\xfa"
    boxes[holder_mapping_entry(holder_mapping_entry(roots["allow"], a1), a2)] = (5).to_bytes(32, "big")
    entry = holder_mapping_entry(roots["accts"], a1)
    boxes[entry] = enc("(uint64,byte[])", [9, b""])
    boxes[holder_mapping_entry(holder_member(entry, 1), a2)] = (50).to_bytes(32, "big")
    group = holder_mapping_entry(roots["members"], (1).to_bytes(32, "big"))
    boxes[group] = enc("(byte[32][],byte[])", [[b32_a2], b""])
    boxes[holder_mapping_entry(holder_member(group, 1), b32_a2)] = (1).to_bytes(32, "big")
    boxes[roots["rows"]] = enc("(uint64,byte[])[2]", [[7, b""], [8, b""]])
    boxes[holder_mapping_entry(holder_member(holder_array_element(roots["rows"], 1), 1), a1)] = (77).to_bytes(32, "big")
    return layout, arc56, boxes, roots


def test_format2_reader_walks_every_holder_shape():
    from chd_storage import NativeStorageReader
    layout, arc56, boxes, roots = _format2_fixture()
    s1, s2 = symbol(1), symbol(10002)
    ev = _evidence({s1: ACCOUNT, s2: ARG})
    ev._numbers.setdefault(1, set())
    sha = lambda data: hashlib.sha256(data).digest()  # noqa: E731
    fold = lambda raw: (s1 if bytes(raw) in (ACCOUNT, NARROWED)  # noqa: E731
                        else s2 if bytes(raw) == ARG else "?")
    reader = NativeStorageReader(layout, arc56, boxes, ev, sha, fold)
    maps = reader.read_maps()
    assert maps["__declared__"] == ["accts", "allow", "bal", "members"]
    assert "__holder_mismatch__" not in maps
    assert maps["bal"] == {s1: 250}
    assert maps["allow"] == {f"{s1}->{s2}": 5}
    # struct value: [tag, {sub-map}] — the mapping member is a nested dict.
    assert maps["accts"] == {s1: [9, {s2: 50}]}
    # transparent wrapper: [[values, positions]] mirrors EvmStorageReader's
    # one-member wrapper list; bytes32 values fold to the address symbol.
    assert maps["members"] == {"#1": [[[s2], {s2: 1}]]}
    # every box except the pure mapping-root placeholders (chained from, never
    # decoded; avm_leg excludes declared roots itself) is attributed, so nothing
    # is left for the unattributed-box check.
    mapping_roots = {roots[name] for name in ("bal", "allow", "accts", "members")}
    assert reader.matched == set(boxes) - mapping_roots
    assert set(reader.format2_roots) == set(roots)


def test_format2_root_coordinate_mismatch_is_reported():
    import base64
    from chd_storage import NativeStorageReader, holder_root
    layout, arc56, boxes, roots = _format2_fixture()
    # The compiler claims slot 9 for `bal`, solc says slot 0.
    wrong = holder_root(9)
    arc56["state"]["keys"]["box"]["bal"]["key"] = base64.b64encode(wrong).decode()
    reader = NativeStorageReader(
        layout, arc56, boxes, _evidence({symbol(1): ACCOUNT}),
        lambda d: hashlib.sha256(d).digest(), lambda raw: "?")
    maps = reader.read_maps()
    [mismatch] = maps["__holder_mismatch__"]
    assert mismatch["root"] == "bal" and mismatch["slot"] == "0"
    assert mismatch["arc56"] == wrong.decode() and mismatch["solc"] == roots["bal"].decode()


def test_legacy_prefix_maps_still_read_when_declared():
    """A pre-format-2 artifact (state.maps.box populated) keeps the old chain."""
    from algosdk import abi
    from chd_storage import NativeStorageReader
    layout, _, _, _ = _format2_fixture()
    arc56 = {"structs": {}, "state": {"maps": {"box": {
        "bal": {"keyType": "AVMBytes", "valueType": "AVMBytes"}}}, "keys": {"box": {}}}}
    sha = lambda d: hashlib.sha256(d).digest()  # noqa: E731
    boxes = {sha(NARROWED + b"bal"): (42).to_bytes(32, "big")}
    reader = NativeStorageReader(layout, arc56, boxes, _evidence({symbol(1): ACCOUNT}),
                                 sha, lambda raw: symbol(1))
    assert reader.read_maps()["bal"] == {symbol(1): 42}
