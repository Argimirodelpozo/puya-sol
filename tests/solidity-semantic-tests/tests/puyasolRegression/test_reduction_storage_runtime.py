"""Runtime helper identity depends on solc host facts, not neighboring roots."""

import json

import pytest
from algosdk.encoding import decode_address

from framework.compile import CompileError
from test_call_operands import invoke

DENSE = """
contract Dense {
    uint256[4] private values;
    function update(uint256 i, uint256 v) external returns (uint256) {
        values[i] = v;
        return values[i];
    }
}
contract DensePages {
    uint256[128] private values;
    function update(uint256 i, uint256 v) external returns (uint256) {
        values[i] = v;
        return values[i];
    }
}
"""
SPARSE = """
library RawSlots {
    function update(uint256 slot, uint256 value) internal returns (uint256 result) {
        assembly { sstore(slot, value) result := sload(slot) }
    }
}
contract Sparse {
    mapping(uint256 => uint256) private values;
    function update(uint256 i, uint256 v) external returns (uint256) {
        values[i] = v;
        return values[i];
    }
    function raw(uint256 slot, uint256 value) external returns (uint256) {
        return RawSlots.update(slot, value);
    }
}
"""


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_storage_variant_independence(harness, tmp_path, via_ir):
    source = tmp_path / "runtime_shapes.sol"
    sizes = []
    for body in (DENSE, DENSE + SPARSE, SPARSE + DENSE):
        source.write_text("pragma solidity ^0.8.24;\n" + body)
        artifacts = harness.compile(source, via_yul_behavior=via_ir,
            extra_args=["--contract-abi", "evm", "--evm-storage-layout"])
        sizes.append(tuple(artifacts.by_contract[name]["approval_teal"].with_suffix(".bin").stat().st_size
                           for name in ("Dense", "DensePages")))
        for name, index in (("Dense", 3), ("DensePages", 127)):
            app = harness.deploy(artifacts, name, fund_wei=20_000_000)
            assert invoke(harness, app, "evm", "update(uint256,uint256)", [index, 71]) == (71,)
        roots = json.loads((harness.out_dir / "awst.json").read_text())
        identities = [root["id"] for root in roots if "id" in root]
        assert len(identities) == len(set(identities)), "duplicate runtime identities"
        if "contract Sparse" in body:
            app = harness.deploy(artifacts, "Sparse", fund_wei=20_000_000)
            assert invoke(harness, app, "evm", "update(uint256,uint256)", [19, 77]) == (77,)
            for slot in (0xFFFF, 0x10000, 2**256 - 1):
                assert invoke(harness, app, "evm", "raw(uint256,uint256)", [slot, 89]) == (89,)
    assert sizes[0] == sizes[1] == sizes[2], sizes


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("recursive", [False, True], ids=["deep", "recursive"])
def test_nested_bool_storage_gate_has_no_depth_cutoff(harness, tmp_path, via_ir, recursive):
    if recursive:
        types = "struct S0 { S0[] children; bool[] flags; }"
        root = "S0"
    else:
        types = "struct S0 { bool[] flags; }\n" + "\n".join(
            f"struct S{i} {{ S{i - 1} child; }}" for i in range(1, 22))
        root = "S21"
    source = tmp_path / "nested_bool.sol"
    source.write_text("pragma solidity ^0.8.24;\ncontract NestedBool {\n" + types
        + f"\n{root} private value;\nfunction probe() external pure returns (uint256) {{ return 1; }}\n}}")
    with pytest.raises(CompileError, match=r"storage `bool\[\]` is unsupported in the default storage mode"):
        harness.compile(source, via_yul_behavior=via_ir)
    # The same solc type graph is valid in the byte-consistent slot backend.
    artifacts = harness.compile(source, via_yul_behavior=via_ir,
        extra_args=["--contract-abi", "evm", "--evm-storage-layout"])
    app = harness.deploy(artifacts, "NestedBool")
    assert invoke(harness, app, "evm", "probe()") == (1,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("array", [False, True], ids=["struct", "fixed-array"])
def test_packed_address_runtime_shape(harness, tmp_path, via_ir, array):
    declaration = "Packed[2] private values;" if array else "Packed private value;"
    value = "values[1]" if array else "value"
    body = f"""
abstract contract PackedBase {{
    struct Packed {{ uint8 tag; address owner; }}
    {declaration}
    function roundTrip(address owner) external returns (address, uint8) {{
        {value}.owner = owner;
        {value}.tag = 17;
        return ({value}.owner, {value}.tag);
    }}
    function clear() external returns (address, uint8) {{
        delete {value};
        return ({value}.owner, {value}.tag);
    }}
}}
contract PackedShape is PackedBase {{}}
"""
    source = tmp_path / "packed_shape.sol"
    for neighbor in (False, True):
        source.write_text("pragma solidity ^0.8.24;\n" + body + (SPARSE if neighbor else ""))
        app = harness.compile_and_deploy(source, "PackedShape", via_yul_behavior=via_ir,
            extra_args=["--evm-storage-layout"], fund_wei=30_000_000)
        result = harness.call(app, "roundTrip(address)", harness.localnet.account.address, extra_fee=40_000)
        assert not result.reverted, result.fail_message
        assert decode_address(result.abi_return[0]) == decode_address(harness.localnet.account.address)
        assert result.abi_return[1] == 17
        result = harness.call(app, "clear()", extra_fee=40_000)
        assert not result.reverted, result.fail_message
        assert decode_address(result.abi_return[0]) == bytes(32)
        assert result.abi_return[1] == 0
