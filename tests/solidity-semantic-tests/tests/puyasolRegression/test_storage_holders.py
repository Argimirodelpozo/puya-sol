"""Solc 0.8.34/Cancun, legacy optimizer-off oracle confirms runtime results."""

import base64
import json

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int
from framework.compile import CompileError
from framework.storage_keys import holder_array_element, holder_member, holder_root, mapping_entry


def _call(harness, app, abi, signature, args=(), returns=("bool",), revert=False):
    if abi == "arc4":
        result = harness.call(app, signature, *args, extra_fee=30_000, expect_revert=revert)
        values = (result.abi_return,) if returns else ()
    else:
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(["uint256"] * len(args), args),),
                                  extra_fee=30_000, budget_pool=14, expect_revert=revert)
        values = decode(returns, result.logs[-1][4:]) if returns and not result.reverted else ()
    assert result.reverted == revert, result.fail_message
    return () if revert else tuple(map(as_int, values))


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_storage_holder_paths(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_storage_holders.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    app = harness.deploy(artifacts, "StorageHolders", exact_schema=True, fund_wei=20_000_000)
    for signature in ("roots()", "nested()", "mappingValues()", "arrays()", "getters()", "effectful()"):
        assert _call(harness, app, abi, signature) == (1,)
    for index, expected in ((0, 401), (1, 402)):
        assert _call(harness, app, abi, "fixedMaps(uint256,uint256)", (index, 3), ("uint256",)) == (expected,)
    for index, expected in ((0, 403), (1, 404)):
        assert _call(harness, app, abi, "dynamicMaps(uint256,uint256,uint256)",
                     (index, 0, 3), ("uint256",)) == (expected,)
    def boxes():
        return {box["name"] for box in harness.localnet.algod.application_boxes(app.app_id).get("boxes", [])}

    before = boxes()
    for index in (2, 2**64, 2**200):
        _call(harness, app, abi, "invalid(uint256)", (index,), ("uint256",), revert=True)
        _call(harness, app, abi, "fixedMaps(uint256,uint256)", (index, 3), ("uint256",), revert=True)
    assert boxes() == before

    if not slot_layout:
        # Full solc root/member coordinates, not source spellings or AST IDs.
        # a occupies slots 0–1, ab 2–3, abc 4, root 5–8, groups 9,
        # fixedHolders 10–13, dynamicHolders 14, fixedMaps 15–16, dynamicMaps 17.
        key = lambda value: value.to_bytes(32, "big")
        checks = {
            mapping_entry(holder_member(holder_root(0), 1), key(1)): 44,
            mapping_entry(holder_member(holder_root(2), 1), key(1)): 55,
            mapping_entry(holder_root(4), key(1)): 33,
            mapping_entry(holder_member(holder_member(holder_root(5), 0), 1), key(1)): 105,
            mapping_entry(holder_member(holder_member(holder_root(5), 2), 1), key(1)): 106,
            mapping_entry(holder_array_element(holder_root(15), 1), key(3)): 402,
            mapping_entry(holder_array_element(holder_array_element(holder_root(17), 1), 0), key(3)): 404,
        }
        for box_name, expected in checks.items():
            box = harness.localnet.algod.application_box_by_name(app.app_id, box_name)
            assert int.from_bytes(base64.b64decode(box["value"]), "big") == expected

    assert _call(harness, app, abi, "references()") == (1,)


def test_storage_holder_manifest(harness):
    artifacts = harness.compile("puyasolRegression/contracts/rev_2_storage_holders.sol")
    roots = {root["name"]: root for root in json.loads((harness.out_dir / "awst.json").read_text())
             if root.get("_type") == "Contract"}

    def binding(contract, name):
        return next(item for item in roots[contract]["app_state"] if item["member_name"] == name)

    # Root identity is unchanged by declaration/type/member renames or AST numbering.
    original = binding("HolderOriginal", "original")
    renamed = binding("HolderRenamed", "renamed")
    assert original["key"]["value"] == renamed["key"]["value"]
    for contract, name, slot in (("HolderOriginal", "original", 0), ("HolderRenamed", "renamed", 0),
                                 ("HolderWide", "values", 2**200), ("StorageHolders", "abc", 4)):
        spec = json.loads(artifacts.by_contract[contract]["arc56"].read_text())
        entry = spec["state"]["keys"]["box"][name]
        assert base64.b64decode(entry["key"]) == holder_root(slot)
        assert "holder format 2" in entry["desc"]
        assert name not in spec["state"]["maps"]["box"]
        assert len(holder_root(slot)) == 54

    for contract, slot in (("HolderOriginal", 0), ("HolderRenamed", 0), ("HolderWide", 2**200)):
        app = harness.deploy(artifacts, contract, exact_schema=True, fund_wei=5_000_000)
        harness.call(app, "write(uint256,uint256)", 7, 17)
        parent = holder_root(slot)
        if contract != "HolderWide":
            parent = holder_member(parent, 1)
        key = mapping_entry(parent, (7).to_bytes(32, "big"))
        value = harness.localnet.algod.application_box_by_name(app.app_id, key)
        assert int.from_bytes(base64.b64decode(value["value"]), "big") == 17


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_storage_holder_alias_lifetime(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_holder_aliases.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    app = harness.deploy(artifacts, "HolderAliases", exact_schema=True, fund_wei=10_000_000)
    assert _call(harness, app, abi, "aliases()") == (1,)
    assert _call(harness, app, abi, "returnedRefs()") == (1,)
    assert _call(harness, app, abi, "reboundAlias()") == (1,)
    assert _call(harness, app, abi, "tupleRebind()") == (1,)


@pytest.mark.parametrize("path", ["array", "field", "return", "named"])
@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_interior_holder_reference(harness, path, slot_layout, abi):
    source = f"puyasolRegression/contracts/rev_2_holder_interior_{path}.sol"
    args = ["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else [])
    if not slot_layout:
        with pytest.raises(CompileError, match="mapping-containing aggregate storage references require a whole-box root"):
            harness.compile(source, extra_args=args)
        return
    artifacts = harness.compile(source, extra_args=args)
    app = harness.deploy(artifacts, "HolderInterior", exact_schema=True, fund_wei=5_000_000)
    assert _call(harness, app, abi, "run()") == (1,)


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_storage_holder_nested_library_reference(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "storage/contracts/storage_ref_returned_nested.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    app = harness.deploy(artifacts, "C", exact_schema=True, fund_wei=5_000_000)
    for value in (42, 99):
        _call(harness, app, abi, "mutate(uint256,uint256,uint256)", (1, 5, value), ())
        assert _call(harness, app, abi, "read(uint256,uint256)", (1, 5), ("uint256",)) == (value,)
    for args in ((1, 6), (2, 5)):
        assert _call(harness, app, abi, "read(uint256,uint256)", args, ("uint256",)) == (0,)


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_storage_holder_transparent_wrappers(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_holder_wrappers.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    app = harness.deploy(artifacts, "HolderWrappers", exact_schema=True, fund_wei=10_000_000)
    assert _call(harness, app, abi, "run()") == (1,)
    if not slot_layout:
        # Solc: roots at slots 0, 3, 6, 7; Data.values is at relative slot 2.
        # Both wrapper ranks are transparent, including inside arrays/mappings.
        key = lambda value: value.to_bytes(32, "big")
        holders = (
            (holder_root(0), 125),
            (holder_root(3), 123),
            (mapping_entry(holder_root(6), key(3)), 111),
            (holder_array_element(holder_root(7), 1), 32),
        )
        for holder, expected in holders:
            box_name = mapping_entry(holder_member(holder, 2), key(1))
            box = harness.localnet.algod.application_box_by_name(app.app_id, box_name)
            assert int.from_bytes(base64.b64decode(box["value"]), "big") == expected


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_storage_slot_bindings(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_slot_bindings.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    app = harness.deploy(artifacts, "SlotBindings", exact_schema=True)
    assert _call(harness, app, abi, "run()") == (1,)
