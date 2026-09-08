"""Facts crossing Yul calls must hold for every caller and every execution."""
import json

import pytest

from framework import as_int

SOURCE = "puyasolRegression/contracts/yul_argument_facts.sol"


@pytest.mark.parametrize("slot_layout", [False, True])
def test_yul_argument_facts(harness, slot_layout):
    app = harness.compile_and_deploy(
        SOURCE, extra_args=["--evm-storage-layout"] if slot_layout else [])
    for value in (0, 37, (1 << 256) - 1):
        for method in ("aligned(uint256)", "constantOffset(uint256)"):
            assert as_int(harness.call(app, method, value).abi_return) == value
        assert tuple(map(as_int, harness.call(app, "reassigned(uint256)", value).abi_return)) == (value, 4095)
        assert tuple(map(as_int, harness.call(app, "snapshot(uint256)", value).abi_return)) == (value, 3583)
        for n in range(4):
            assert as_int(harness.call(app, "recursive(uint256,uint256)", n, value).abi_return) == value
        for ptr in (512, 4095, 8161):
            assert as_int(harness.call(app, "memoryPointer(uint256,uint256)", ptr, value).abi_return) == value
    for method in ("mixed()", "sameResidue()"):
        assert tuple(map(as_int, harness.call(app, method).abi_return)) == (11, 22)


def test_yul_argument_fact_lowering(harness):
    harness.compile(SOURCE)
    roots = json.loads((harness.out_dir / "awst.json").read_text())
    subs = {root["id"].split("::__yul_")[-1]: root for root in roots
            if root.get("_type") == "Subroutine" and "::__yul_" in root["id"]}
    read = "__puyasol_memory_read_word"
    for name in ("read_aligned", "read_constant", "read_same_residue"):
        assert read not in json.dumps(subs[name]["body"]), name
    for name in ("read_mixed", "read_reassigned", "read_snapshot", "read_recursive", "read_memory_pointer"):
        assert read in json.dumps(subs[name]["body"]), name
