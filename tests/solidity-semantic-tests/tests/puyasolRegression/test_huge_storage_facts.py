"""Solc layout facts are full-width even when whole values cannot fit on AVM."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int
from framework.compile import CompileError


def _call(harness, app, abi, signature, args=(), result_count=0, revert=False):
    if abi == "arc4":
        result = harness.call(app, signature, *args, extra_fee=20_000, expect_revert=revert)
        values = ((result.abi_return,) if result_count == 1 else result.abi_return) if result_count else ()
    else:
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(["uint256"] * len(args), args),),
                                  extra_fee=20_000, expect_revert=revert)
        values = decode(["uint256"] * result_count, result.logs[-1][4:]) if result_count and not result.reverted else ()
    assert result.reverted == revert, result.fail_message
    return () if revert else tuple(map(as_int, values))


def _boxes(harness, app):
    return {box["name"] for box in harness.localnet.algod.application_boxes(app.app_id).get("boxes", [])}


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_huge_layout_facts_do_not_materialize_arrays(harness, slot_layout, abi):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_huge_layout_facts.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )
    app = harness.deploy(artifacts, exact_schema=True)
    boxes = _boxes(harness, app)
    layout = (2**200, 0, 1, 1 + 2**195)
    assert _call(harness, app, abi, "facts()", result_count=6) == (*layout, 7, 9)
    assert _boxes(harness, app) == boxes
    _call(harness, app, abi, "change()")
    assert _call(harness, app, abi, "facts()", result_count=6) == (*layout, 11, 13)


def test_huge_named_cell_access_has_capacity_diagnostic(harness):
    with pytest.raises(CompileError, match="exceeds the compiler's addressable range"):
        harness.compile("puyasolRegression/contracts/rev_2_huge_sparse_array.sol")


@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_huge_sparse_elements_use_full_width_slots(harness, abi):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_huge_sparse_array.sol",
        extra_args=["--contract-abi", abi, "--evm-storage-layout"], fund_wei=1_000_000,
    )
    indices = (0, 2**130, 2**200 - 1)
    boxes = _boxes(harness, app)
    for index in indices:
        assert _call(harness, app, abi, "values(uint256)", (index,), 1) == (0,)
    assert _boxes(harness, app) == boxes
    for index, value in zip(indices, (11, 22, 33)):
        _call(harness, app, abi, "set(uint256,uint256)", (index, value))
    for index, value in zip(indices, (11, 22, 33)):
        assert _call(harness, app, abi, "values(uint256)", (index,), 1) == (value,)
    for index in (2**200, 2**255):
        _call(harness, app, abi, "values(uint256)", (index,), 1, revert=True)
        _call(harness, app, abi, "set(uint256,uint256)", (index, 99), revert=True)
