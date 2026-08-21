from __future__ import annotations

import sys
from pathlib import Path

import pytest
from algosdk import abi

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from chd_common import (build_dep_tape_plans, bytes32_mapping_key_candidates,
                        call_without_consuming_tapes, dump_json, relax_pragma)
from chd_slot_reader import _kec, read_slot_storage
from chd_storage import (EvmStorageReader, KeyEvidence, NativeStorageReader,
                         build_parameterized_getter_probes)
import fetch as fetch_module
from fetch import _decode_ctor_addresses, _proxy_constructor_setup


def test_constructor_dependency_decoder_recurses_through_tuple_arrays() -> None:
    addresses = [f"0x{i:040x}" for i in range(1, 5)]
    abi = [{
        "type": "constructor",
        "inputs": [{
            "name": "params",
            "type": "tuple",
            "components": [
                {"name": "admin", "type": "address"},
                {"name": "tokens", "type": "address[2]"},
                {"name": "nested", "type": "tuple", "components": [
                    {"name": "receiver", "type": "address"},
                ]},
            ],
        }],
    }]
    # The entire tuple is static, so its encoding is the four address words.
    ctor_hex = "".join("0" * 24 + a[2:] for a in addresses)

    assert _decode_ctor_addresses(abi, ctor_hex) == addresses


def test_proxy_constructor_setup_is_shape_agnostic_and_rejects_ambiguity() -> None:
    sc = {"decoded_constructor_args": [
        ["0x1111111111111111111111111111111111111111",
         {"name": "_logic", "type": "address"}],
        ["0x2222222222222222222222222222222222222222",
         {"name": "initialOwner", "type": "address"}],
        ["0x12345678" + "00" * 32, {"name": "_data", "type": "bytes"}],
    ]}
    assert _proxy_constructor_setup(sc) == (
        "0x1111111111111111111111111111111111111111",
        "0x12345678" + "00" * 32,
    )

    sc["decoded_constructor_args"].append(
        ["0xabcdef01", {"name": "secondData", "type": "bytes"}])
    assert _proxy_constructor_setup(sc)[1] is None


def test_internal_parent_pagination_stops_on_repeated_full_page(monkeypatch) -> None:
    parent_hash = "0x" + "12" * 32
    row = {
        "transactionHash": parent_hash,
        "blockNumber": "0x64",
        "transactionIndex": "0x1",
        "timeStamp": "0x2",
    }
    calls = {"logs": 0, "trace": 0}

    def fake_http(url: str, timeout: int = 40):
        if "action=tokentx" in url:
            return {"result": []}
        if "action=getLogs" in url:
            calls["logs"] += 1
            return {"result": [row] * 1000}
        if "/raw-trace" in url:
            calls["trace"] += 1
            return []
        raise AssertionError(url)

    monkeypatch.setattr(fetch_module, "http_json", fake_http)
    monkeypatch.setattr(fetch_module.time, "sleep", lambda _: None)

    assert fetch_module.fetch_internal_calls(
        "explorer.invalid", "0x" + "34" * 20, 1, 200,
        set(), max_parents=5, max_calls=5) == []
    assert calls == {"logs": 2, "trace": 1}


def test_internal_dependency_harvest_excludes_delegatecall(monkeypatch) -> None:
    target = "0x" + "34" * 20
    spoke = "0x" + "56" * 20
    implementation = "0x" + "78" * 20
    authority = "0x" + "9a" * 20
    parent_hash = "0x" + "12" * 32
    log_row = {
        "transactionHash": parent_hash,
        "blockNumber": "0x64",
        "transactionIndex": "0x1",
        "timeStamp": "0x2",
    }
    trace = [
        {"type": "call", "traceAddress": [0],
         "action": {"from": spoke, "to": target, "callType": "call",
                    "input": "0x12345678", "value": "0x0"},
         "result": {"output": "0x"}},
        {"type": "call", "traceAddress": [0, 0],
         "action": {"from": target, "to": implementation,
                    "callType": "delegatecall", "input": "0xabcdef01"},
         "result": {"output": "0x01"}},
        {"type": "call", "traceAddress": [0, 1],
         "action": {"from": target, "to": authority,
                    "callType": "staticcall", "input": "0xfeedface"},
         "result": {"output": "0x02"}},
    ]

    def fake_http(url: str, timeout: int = 40):
        if "action=tokentx" in url:
            return {"result": []}
        if "action=getLogs" in url:
            return {"result": [log_row]}
        if "/raw-trace" in url:
            return trace
        raise AssertionError(url)

    monkeypatch.setattr(fetch_module, "http_json", fake_http)
    monkeypatch.setattr(fetch_module.time, "sleep", lambda _: None)
    callees, tapes = {}, {}
    got = fetch_module.fetch_internal_calls(
        "explorer.invalid", target, 1, 200, set(), max_parents=5,
        max_calls=5, callee_sink=callees, tape_sink=tapes)

    assert len(got) == 1
    assert callees == {authority: 1}
    assert set(tapes) == {authority}
    assert tapes[authority][0]["sel"] == "0xfeedface"


def test_composite_bytes32_key_uses_solidity_packed_width() -> None:
    token = "ab" * 32
    hashed = "cd" * 32
    seen = []

    def fake_keccak(data: bytes) -> bytes:
        seen.append(data)
        return bytes.fromhex(hashed)

    calls = [{
        "sig": "link(address,uint32,bytes32)",
        "args": [{"__addr__": 1}, 7, {"__b__": token}],
    }]
    fns = {"link(address,uint32,bytes32)": {"inputs": [
        {"type": "address"}, {"type": "uint32"}, {"type": "bytes32"},
    ]}}

    assert bytes32_mapping_key_candidates(calls, fns, fake_keccak) == [
        token, hashed,
    ]
    assert seen == [(7).to_bytes(4, "big") + bytes.fromhex(token)]


def test_slot_reader_covers_dependency_and_composite_mapping_keys() -> None:
    dep = bytes.fromhex("11" * 32)
    token = bytes.fromhex("22" * 32)
    domain = 7
    composite = _kec(domain.to_bytes(4, "big") + token)
    local = bytes.fromhex("33" * 32)

    def mapping_slot(key: bytes, root: int) -> int:
        return int.from_bytes(_kec(key + root.to_bytes(32, "big")), "big")

    layout = {
        "storage": [
            {"label": "limits", "slot": "0", "offset": 0,
             "type": "map_address_uint"},
            {"label": "remotes", "slot": "1", "offset": 0,
             "type": "map_bytes32_address"},
        ],
        "types": {
            "map_address_uint": {
                "encoding": "mapping", "label": "mapping(address => uint256)",
                "key": "address", "value": "uint256",
            },
            "map_bytes32_address": {
                "encoding": "mapping", "label": "mapping(bytes32 => address)",
                "key": "bytes32", "value": "address",
            },
            "uint256": {"encoding": "inplace", "label": "uint256",
                        "numberOfBytes": "32"},
            "address": {"encoding": "inplace", "label": "address",
                        "numberOfBytes": "20"},
            "bytes32": {"encoding": "inplace", "label": "bytes32",
                        "numberOfBytes": "32"},
        },
    }
    calls = [{
        "sig": "link(address,uint32,bytes32)",
        "args": [{"__dep__": "0xdead"}, domain, {"__b__": token.hex()}],
    }]
    fns = {"link(address,uint32,bytes32)": {"inputs": [
        {"type": "address"}, {"type": "uint32"}, {"type": "bytes32"},
    ]}}
    slots = {
        mapping_slot(dep, 0): (123).to_bytes(32, "big"),
        mapping_slot(composite, 1): local,
    }

    def fold(value):
        return "«D0»" if value == "0x" + local.hex() else value

    storage = read_slot_storage(
        slots, layout, {"«D0»": dep}, fold, calls, fns)

    assert storage["maps"]["limits"] == {"«D0»": 123}
    assert storage["maps"]["remotes"] == {"0x" + composite.hex(): "«D0»"}
    assert storage["maps"]["__unattributed_boxes__"] == 0


def test_recursive_native_and_evm_readers_cover_aave_shaped_state() -> None:
    """One type walk handles structs, nested maps, arrays, and map-in-struct."""
    from chd_common import symbol
    import hashlib

    types = {
        "uint": {"encoding": "inplace", "label": "uint256",
                 "numberOfBytes": "32"},
        "addr": {"encoding": "inplace", "label": "address",
                 "numberOfBytes": "20"},
        "b32": {"encoding": "inplace", "label": "bytes32",
                "numberOfBytes": "32"},
        "asset": {"encoding": "inplace", "label": "struct I.Asset",
                  "numberOfBytes": "32", "members": [
                      {"label": "amount", "slot": "0", "offset": 0,
                       "type": "uint"}]},
        "spoke": {"encoding": "inplace", "label": "struct I.Spoke",
                  "numberOfBytes": "32", "members": [
                      {"label": "debt", "slot": "0", "offset": 0,
                       "type": "uint"}]},
        "values": {"encoding": "dynamic_array", "label": "bytes32[]",
                   "numberOfBytes": "32", "base": "b32"},
        "positions": {"encoding": "mapping", "label": "mapping(bytes32 => uint256)",
                      "numberOfBytes": "32", "key": "b32", "value": "uint"},
        "set": {"encoding": "inplace", "label": "struct E.Set",
                "numberOfBytes": "64", "members": [
                    {"label": "_values", "slot": "0", "offset": 0,
                     "type": "values"},
                    {"label": "_positions", "slot": "1", "offset": 0,
                     "type": "positions"}]},
        "address_set": {"encoding": "inplace", "label": "struct E.AddressSet",
                        "numberOfBytes": "64", "members": [
                            {"label": "_inner", "slot": "0", "offset": 0,
                             "type": "set"}]},
        "assets": {"encoding": "mapping", "label": "mapping(uint256 => struct I.Asset)",
                   "numberOfBytes": "32", "key": "uint", "value": "asset"},
        "spokes_inner": {"encoding": "mapping",
                         "label": "mapping(address => struct I.Spoke)",
                         "numberOfBytes": "32", "key": "addr", "value": "spoke"},
        "spokes": {"encoding": "mapping",
                   "label": "mapping(uint256 => mapping(address => struct I.Spoke))",
                   "numberOfBytes": "32", "key": "uint", "value": "spokes_inner"},
        "sets": {"encoding": "mapping",
                 "label": "mapping(uint256 => struct E.AddressSet)",
                 "numberOfBytes": "32", "key": "uint", "value": "address_set"},
    }
    layout = {"types": types, "storage": [
        {"label": "_assets", "slot": "1", "offset": 0, "type": "assets"},
        {"label": "_spokes", "slot": "2", "offset": 0, "type": "spokes"},
        {"label": "_sets", "slot": "3", "offset": 0, "type": "sets"},
    ]}
    arc56 = {"structs": {
        "Asset": [{"name": "amount", "type": "uint256"}],
        "Spoke": [{"name": "debt", "type": "uint256"}],
        "AddressSet": [{"name": "_inner", "type": "Set"}],
        "Set": [{"name": "_values", "type": "byte[32][]"},
                {"name": "_positions", "type": "byte[]"}],
    }, "state": {"maps": {"box": {
        "_assets": {"keyType": "AVMBytes", "valueType": "Asset"},
        "_spokes": {"keyType": "AVMBytes", "valueType": "Spoke"},
        "_sets": {"keyType": "AVMBytes", "valueType": "AddressSet"},
    }}}}
    calls = [{"i": 0, "sig": "touch(uint256,address)",
              "sender": {"__addr__": 1},
              "args": [7, {"__addr__": 1}]}]
    fns = {"touch(uint256,address)": {"inputs": [
        {"type": "uint256"}, {"type": "address"}], "outputs": []}}
    label = symbol(1)
    evm_addr = bytes.fromhex("12" * 20).rjust(32, b"\0")
    avm_addr = bytes.fromhex("34" * 32)

    evm_words = {}
    asset_slot = int.from_bytes(_kec((7).to_bytes(32, "big")
                                     + (1).to_bytes(32, "big")), "big")
    evm_words[asset_slot] = (11).to_bytes(32, "big")
    spoke_outer = _kec((7).to_bytes(32, "big") + (2).to_bytes(32, "big"))
    spoke_slot = int.from_bytes(_kec(evm_addr[-20:].rjust(32, b"\0")
                                     + spoke_outer), "big")
    evm_words[spoke_slot] = (22).to_bytes(32, "big")
    set_slot = int.from_bytes(_kec((7).to_bytes(32, "big")
                                   + (3).to_bytes(32, "big")), "big")
    evm_words[set_slot] = (1).to_bytes(32, "big")
    values_base = int.from_bytes(_kec(set_slot.to_bytes(32, "big")), "big")
    evm_words[values_base] = evm_addr
    position_slot = int.from_bytes(_kec(
        evm_addr + (set_slot + 1).to_bytes(32, "big")), "big")
    evm_words[position_slot] = (1).to_bytes(32, "big")

    evm_evidence = KeyEvidence(calls, fns, {label: evm_addr})
    evm = EvmStorageReader(
        layout, lambda slot: evm_words.get(slot, bytes(32)),
        evm_evidence, _kec, set(evm_words)).read(
            lambda raw: label if bytes.fromhex(str(raw)[2:]) == evm_addr else "?" + str(raw))

    def sha(data):
        return hashlib.sha256(data).digest()
    asset_box = sha((7).to_bytes(32, "big") + b"_assets")
    spoke_box = sha(avm_addr + sha((7).to_bytes(32, "big") + b"_spokes"))
    set_prefix = sha((7).to_bytes(32, "big") + b"_sets") + b"_inner"
    boxes = {
        asset_box: abi.ABIType.from_string("(uint256)").encode([11]),
        spoke_box: abi.ABIType.from_string("(uint256)").encode([22]),
        set_prefix: abi.ABIType.from_string("(byte[32][],byte[])").encode(
            [[list(avm_addr)], []]),
        sha(avm_addr + set_prefix + b"_positions"): (1).to_bytes(1, "big"),
    }
    avm_evidence = KeyEvidence(calls, fns, {label: avm_addr})
    native_reader = NativeStorageReader(
        layout, arc56, boxes, avm_evidence, sha,
        lambda raw: label if bytes(raw) == avm_addr else "?" + str(raw))
    native = native_reader.read_maps()

    assert evm["maps"]["_assets"] == native["_assets"] == {"#7": [11]}
    assert evm["maps"]["_spokes"] == native["_spokes"] == {
        f"#7->{label}": [22]}
    assert evm["maps"]["_sets"] == native["_sets"] == {
        "#7": [[[label], {label: 1}]]
    }
    assert native_reader.matched == set(boxes)


def test_parameterized_getter_probes_are_type_and_context_driven() -> None:
    abi_doc = [{"type": "function", "name": "lookup",
                "stateMutability": "view",
                "inputs": [{"type": "uint256"}, {"type": "address"}],
                "outputs": [{"type": "uint256"}]}]
    calls = [{"sig": "write(uint256)", "sender": {"__addr__": 4},
              "args": [9]}]
    fns = {"write(uint256)": {"inputs": [{"type": "uint256"}], "outputs": []}}

    probes = build_parameterized_getter_probes(abi_doc, calls, fns)

    assert probes == [{"sig": "lookup(uint256,address)",
                       "args": [9, {"__addr__": 4}],
                       "outputs": [{"type": "uint256"}],
                       "source_txn": 0}]


def test_read_only_preflight_restores_python_tape_cursors() -> None:
    cursors = {b"dependency-a": 3, b"dependency-b": 8}

    def consume():
        cursors[b"dependency-a"] += 2
        cursors[b"dependency-c"] = 1
        return "result"

    assert call_without_consuming_tapes(consume, cursors) == "result"
    assert cursors == {b"dependency-a": 3, b"dependency-b": 8}

    def consume_then_revert():
        cursors[b"dependency-b"] += 1
        raise RuntimeError("revert")

    with pytest.raises(RuntimeError, match="revert"):
        call_without_consuming_tapes(consume_then_revert, cursors)
    assert cursors == {b"dependency-a": 3, b"dependency-b": 8}


def test_dependency_tape_plan_is_selector_and_transaction_bounded(tmp_path) -> None:
    dep = "0x" + "12" * 20
    calls = [
        {"i": 0, "hash": "0xaaa#0.1"},
        {"i": 1, "hash": "0xbbb#0.1"},
    ]
    dump_json(tmp_path / "dep_tape.json", {"tapes": {dep: [
        {"hash": "0xaaa#0.1", "sel": "0x70a08231", "out": "00" * 32},
        {"hash": "0xaaa#0.1", "sel": "0xa9059cbb", "out": "00" * 31 + "01"},
        {"hash": "0xbbb#0.1", "sel": "0xdeadbeef", "out": ""},
    ]}})

    plan = build_dep_tape_plans(tmp_path, set(), calls=calls)[dep]
    assert plan["selectors"] == [
        bytes.fromhex("70a08231"),
        bytes.fromhex("a9059cbb"),
        bytes.fromhex("deadbeef"),
    ]
    assert plan["bounds"] == {0: (0, 2), 1: (2, 3)}
    assert [len(answer) for answer in plan["answers"]] == [32, 32, 0]

def test_pre08_relaxation_is_explicit_and_updates_old_context_signature() -> None:
    source = """
pragma solidity 0.7.6;
abstract contract Context {
    function _msgSender() internal view virtual returns (address payable) {
        return msg.sender;
    }
}
"""
    assert "pragma solidity 0.7.6;" in relax_pragma(source)
    relaxed = relax_pragma(source, pre08=True)
    assert "pragma solidity ^0.8.0;" in relaxed
    assert "returns (address)" in relaxed
    assert "address payable" not in relaxed
