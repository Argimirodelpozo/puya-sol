"""Fresh loop prefixes and single-node solc visitor dispatch."""

import json

import pytest

from framework import as_bytes, as_int


def nodes(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from nodes(child)
    elif isinstance(value, list):
        for child in value:
            yield from nodes(child)


def single_eval_ids(value):
    return {node["_id"] for node in nodes(value) if node.get("_type") == "SingleEvaluation"}


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_loop_prefixes(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/loop_prefixes.sol", contract_name="LoopPrefixes",
        via_yul_behavior=via_ir, extra_args=["--evm-storage-layout"] if slot_layout else [])
    for mask in range(4):
        for stop in (0, 2, 4):
            total = sum(i for i in range(1, stop + 1)
                        if not (i == 1 and mask & 1 or i == 3 and mask & 2))
            result = harness.call(app, "forPrefixes(uint64,uint64)", mask, stop).abi_return
            assert (as_int(result[0]), as_int(result[1]), list(map(as_int, result[2]))) == (
                total, stop, [i if i <= stop else 0 for i in range(1, 5)])
        result = harness.call(app, "doPrefixes(uint64)", mask).abi_return
        assert tuple(map(as_int, result)) == (4, 4, 4, 10 - bool(mask & 1) - 2 * bool(mask & 2))
    assert tuple(map(as_int, harness.call(app, "nested()").abi_return)) == (38, 15)
    for signature in ("forChecked()", "doChecked()"):
        assert harness.call(app, signature, expect_revert=True).reverted
    for signature in ("forUnchecked()", "doUnchecked()"):
        assert as_int(harness.call(app, signature).abi_return) == 1

    # Runtime checks alone can miss backend identity reuse across mutually
    # exclusive arms: both continue prefixes and the tail need distinct IDs.
    awst = json.loads((harness.out_dir / "awst.json").read_text())
    for name in ("forPrefixes", "doPrefixes"):
        method = next(node for node in nodes(awst)
                      if node.get("_type") == "ContractMethod" and node.get("member_name") == name)
        loop = next(node for node in nodes(method["body"]) if node.get("_type") == "WhileLoop")
        expansions = []
        for block in nodes(loop["loop_body"]):
            if block.get("_type") == "Block":
                for i, statement in enumerate(block["body"]):
                    if statement.get("_type") == "LoopContinue":
                        assert i > 0
                        expansions.append(block["body"][i - 1])
        expansions.append(loop["loop_body"]["body"][-1])
        assert len(expansions) == 3, name
        seen = set()
        for expansion in expansions:
            ids = single_eval_ids(expansion)
            assert ids, (name, "fixture must exercise SingleEvaluation")
            assert not seen & ids, (name, "prefix identity reused", seen & ids)
            seen.update(ids)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_solc_visitor_dispatch(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/visitor_dispatch.sol", contract_name="VisitorDispatch",
        via_yul_behavior=via_ir, extra_args=["--evm-storage-layout"] if slot_layout else [])
    # solc 0.8.34: binary operand order differs between legacy and via-IR;
    # short-circuit arms stay conditional and each child is lowered once.
    for choose in (False, True):
        values, trace = harness.call(app, "expressions(bool)", choose).abi_return
        assert list(map(as_int, values)) == [8, 13 if choose else 4, 6]
        assert as_int(trace) == ((2341678 if via_ir else 4321678) if choose else 51679)
    data = b"\x00\xff\x02\x03"
    for start, end in ((0, 4), (1, 3), (2, 2), (4, 4), (3, 2), (0, 5), (17, 17)):
        fail = not start <= end <= len(data)
        result = harness.call(app, "slices(byte[],uint256,uint256)", data, start, end, expect_revert=fail)
        if fail:
            assert result.reverted
        else:
            assert as_bytes(result.abi_return) == data[start:end]
