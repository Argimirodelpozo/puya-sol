"""Solc-shaped copies, binding identity, sparse strides and physical placement."""

import json

import pytest

from framework.compile import CompileError
from test_call_operands import invoke


def compile_facts(harness, name, profile, via_ir=False, slot=False):
    return harness.compile(
        f"puyasolRegression/contracts/storage_{name}_facts.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile]
        + (["--evm-storage-layout"] if slot else []),
    )


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_nested_storage_copy_facts(harness, via_ir, profile, slot):
    artifacts = compile_facts(harness, "copy", profile, via_ir, slot)
    arrays = harness.deploy(artifacts, "ArrayCopyFacts", fund_wei=20_000_000)
    for _ in range(2):
        assert invoke(harness, arrays, profile, "copy()", returns=["uint256"] * 4) == (11, 0, 0, 0)
        assert invoke(harness, arrays, profile, "selfCopy()", returns=["uint256"] * 2) == (1, 17)
    structs = harness.deploy(artifacts, "StructCopyFacts", fund_wei=20_000_000)
    for _ in range(2):
        assert invoke(harness, structs, profile, "copy()",
                      returns=["uint16", "uint256", "uint256", "bytes"]) == (7, 19, 0, b"\x01\x02\x03")


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_packed_storage_copy_facts(harness, via_ir, profile):
    artifacts = compile_facts(harness, "packed_copy", profile, via_ir, slot=True)
    app = harness.deploy(artifacts, "PackedCopyFacts", fund_wei=20_000_000)
    for word in (0, (0x55 << 248) | 0x0807, 2**256 - 1):
        assert invoke(harness, app, profile, "packed(uint256)", [word],
                      ["uint256"] * 3) == (word & 65535, 0, 0)
        assert invoke(harness, app, profile, "wide(uint256)", [word]) == (word % 2**160,)
        assert invoke(harness, app, profile, "selfCopy(uint256)", [word]) == (word,)
        assert invoke(harness, app, profile, "structPadding(uint256)", [word]) == (
            (word & ~(2**24 - 1)) | 0x0201,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_inherited_immutable_identity(harness, via_ir, profile, slot):
    artifacts = compile_facts(harness, "immutable", profile, via_ir, slot)
    for name in ("ImmutableLR", "ImmutableRL"):
        app = harness.deploy(artifacts, name, exact_schema=True)
        assert invoke(harness, app, profile, "both()", returns=["uint256"] * 2) == (11, 22)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_full_width_storage_stride(harness, via_ir, profile):
    artifacts = compile_facts(harness, "stride", profile, via_ir, slot=True)
    app = harness.deploy(artifacts, "StorageStrideFacts", fund_wei=20_000_000)
    coordinates = ((0, 0), (1, 0), (1, 2**38 - 1))
    boxes = harness.localnet.algod.application_boxes(app.app_id)
    for i, j in coordinates:
        assert invoke(harness, app, profile, "get(uint256,uint256)", [i, j], ["uint8"]) == (0,)
    assert harness.localnet.algod.application_boxes(app.app_id) == boxes
    for (i, j), value in zip(coordinates, (11, 22, 33), strict=True):
        assert invoke(harness, app, profile, "set(uint256,uint256,uint8)", [i, j, value], ["uint8"]) == (value,)
    for (i, j), value in zip(coordinates, (11, 22, 33), strict=True):
        assert invoke(harness, app, profile, "get(uint256,uint256)", [i, j], ["uint8"]) == (value,)
    for i, j in ((2, 0), (1, 2**38), (2**64, 0), (0, 2**128)):
        invoke(harness, app, profile, "get(uint256,uint256)", [i, j], reverts=True)
        invoke(harness, app, profile, "set(uint256,uint256,uint8)", [i, j, 7], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_storage_physical_size_policy(harness, via_ir, profile, slot):
    artifacts = compile_facts(harness, "placement", profile, via_ir, slot)
    spec = json.loads((harness.out_dir / "StoragePlacementFacts.arc56.json").read_text())
    cells = spec["state"]["keys"]
    if slot:
        for name in ("values", "bits", "small", "large", "fits", "boxed"):
            assert name not in cells["global"] and name not in cells["box"]
    else:
        for name in ("values", "bits", "small", "fits"):
            assert name in cells["global"] and name not in cells["box"]
        for name in ("large", "boxed"):
            assert name in cells["box"] and name not in cells["global"]
    app = harness.deploy(artifacts, "StoragePlacementFacts", exact_schema=True, fund_wei=20_000_000)
    assert invoke(harness, app, profile, "values(uint256)", [63], ["uint8"]) == (0,)
    assert invoke(harness, app, profile, "update(uint256,uint8)", [63, 13]) == (28,)
    assert invoke(harness, app, profile, "values(uint256)", [63], ["uint8"]) == (13,)
    assert invoke(harness, app, profile, "bits(uint256)", [63], ["bool"]) == (True,)
    assert invoke(harness, app, profile, "boundary(uint248)", [17]) == (34,)
    for index in (64, 2**64 + 1, 2**128):
        invoke(harness, app, profile, "values(uint256)", [index], reverts=True)
        invoke(harness, app, profile, "update(uint256,uint8)", [index, 1], reverts=True)
    assert invoke(harness, app, profile, "clear()") == (0,)
    for name in ("fits", "boxed"):
        assert invoke(harness, app, profile, name + "(uint256)", [3], ["uint248"]) == (0,)


def test_removed_storage_placement_flag_is_rejected(harness):
    with pytest.raises(CompileError, match="Unknown option: --compact-storage-layout"):
        harness.compile("puyasolRegression/contracts/storage_placement_facts.sol",
                        extra_args=["--compact-storage-layout"])


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
@pytest.mark.parametrize("mapped", [False, True], ids=["global-value", "mapping-value"])
def test_storage_placement_reference_passing(harness, via_ir, profile, slot, mapped):
    artifacts = compile_facts(harness, "placement_refs", profile, via_ir, slot)
    name = "StoragePlacementMappedReferenceFacts" if mapped else "StoragePlacementReferenceFacts"
    if not slot:
        spec = json.loads((harness.out_dir / (name + ".arc56.json")).read_text())
        assert ("value" in spec["state"]["keys"]["global"]) == (not mapped)
        assert ("value" in spec["state"]["keys"]["box"]) == mapped
    app = harness.deploy(artifacts, name, exact_schema=True, fund_wei=20_000_000)
    expected = (8, 18, 8, 18) if mapped else (8, 9, 9)
    for _ in range(2):
        assert invoke(harness, app, profile, "run()", returns=["uint256"] * len(expected)) == expected


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_storage_runtime_reachability(harness, profile, slot):
    artifacts = compile_facts(harness, "runtime", profile, slot=slot)
    app = harness.deploy(artifacts, "StorageRuntimeFacts", fund_wei=20_000_000)
    assert invoke(harness, app, profile, "update(uint256)", [17]) == (35,)
    assert invoke(harness, app, profile, "read()", returns=["uint256"] * 2) == (17, 0)
    awst = json.loads((harness.out_dir / "awst.json").read_text())
    storage = [node for node in awst if node.get("id") == "__puyasol___storage_write"]
    if not slot:
        assert not storage, "unreachable assembly requested a default-layout slot dispatcher"
    artifacts = compile_facts(harness, "modifier", profile, slot=slot)
    app = harness.deploy(artifacts, "StorageModifierFacts", fund_wei=20_000_000)
    assert invoke(harness, app, profile, "read()") == (77,)
