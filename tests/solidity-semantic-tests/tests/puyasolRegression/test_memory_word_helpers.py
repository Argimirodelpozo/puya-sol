"""Shared scratch operations retain bounds, neighboring bytes and memory effects."""
import json

import pytest

from framework import as_int

SOURCE = "puyasolRegression/contracts/memory_word_helpers.sol"


@pytest.mark.parametrize("pages", [1, 2, 5])
def test_shared_memory_words(harness, pages):
    app = harness.compile_and_deploy(SOURCE, extra_args=["--evm-memory-slots", str(pages)])
    cap = pages * 4096
    mask = (1 << 256) - 1
    offsets = [512, 4000]
    if pages > 1:
        offsets += [4064, 4065, 4095, 4096]
    if pages > 2:
        offsets += [8161]
    for offset in offsets:
        for value in (0, 37, mask):
            result = harness.call(app, "roundTrip(uint256,uint256)", offset, value,
                                  extra_fee=10_000).abi_return
            assert tuple(map(as_int, result)) == (value, mask ^ value, 0xa55a)
    assert as_int(harness.call(app, "word(uint256,uint256)", cap - 32, mask).abi_return) == mask
    for method, args in (("word(uint256,uint256)", (cap - 31, 1)),
                         ("read(uint256)", (cap - 31,))):
        assert harness.call(app, method, *args, expect_revert=True).reverted
    assert as_int(harness.call(app, "word(uint256,uint256)", 512, 43).abi_return) == 43

    roots = json.loads((harness.out_dir / "awst.json").read_text())
    helpers = [root for root in roots if root.get("id", "").startswith("__puyasol_memory_")]
    assert {root["id"] for root in helpers} == {
        "__puyasol_memory_read_word", "__puyasol_memory_write_word"}
    assert len(helpers) == 2
    assert all(root["inline"] is False and not root["pure"] for root in helpers)
