from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from chd_common import (bytes32_mapping_key_candidates,
                        relax_pragma, should_intercept_dependency_call)
from chd_slot_reader import _kec, read_slot_storage


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


def test_tape_interceptor_passes_through_stub_selectors() -> None:
    mint = bytes.fromhex("40c10f19")
    burn = bytes.fromhex("42966c68")
    assert not should_intercept_dependency_call([b""], mint, {mint})
    assert should_intercept_dependency_call([b""], burn, {mint})
    assert not should_intercept_dependency_call(None, burn, set())


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
