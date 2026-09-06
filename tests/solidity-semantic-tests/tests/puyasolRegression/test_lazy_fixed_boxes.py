"""Absent fixed boxes default at bounded projections, without allocating state."""

import base64

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_bytes, as_int
from framework.compile import CompileError


def _call(harness, app, abi, sig, args=(), returns=(), revert=False):
    if abi == "arc4":
        result = harness.call(app, sig, *args, extra_fee=30_000, expect_revert=revert)
        values = (result.abi_return,) if len(returns) == 1 else result.abi_return
    else:
        selector = keccak.new(digest_bits=256, data=sig.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(["uint256"] * len(args), args),),
                                  extra_fee=30_000, budget_pool=14, expect_revert=revert)
        values = decode(returns, result.logs[-1][4:]) if returns and not result.reverted else ()
    assert result.reverted == revert, result.fail_message
    if revert or not returns:
        return ()
    normalized = []
    for t, v in zip(returns, values):
        value = as_bytes(v) if t.startswith("bytes") else as_int(v)
        # ARC4 exposes signed returns in the compiler's canonical uint256
        # carrier; EVM ABI decoding already supplies a signed Python integer.
        if abi == "arc4" and t.startswith("int") and value >= 2**255:
            value -= 2**256
        normalized.append(value)
    return tuple(normalized)


def _boxes(harness, app):
    return {box["name"] for box in harness.localnet.algod.application_boxes(app.app_id).get("boxes", [])}


@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_lazy_fixed_mapping_windows(harness, abi, via_yul_behavior):
    app = harness.compile_and_deploy("puyasolRegression/contracts/rev_2_lazy_fixed_boxes.sol",
                                     extra_args=["--contract-abi", abi], fund_wei=50_000_000,
                                     via_yul_behavior=via_yul_behavior)
    def call(sig, *args, returns=("uint256",), revert=False):
        return _call(harness, app, abi, sig, args, returns, revert)

    empty_pair = (0, 0, 0, bytes(3))
    pair_types = ("int128", "uint16", "bool", "bytes3")
    boxes = _boxes(harness, app)
    for key in (7, 9):
        for sig, index in (("edge(uint256,uint256)", 127), ("above(uint256,uint256)", 128),
                           ("cap(uint256,uint256)", 1023), ("readAbove(uint256,uint256)", 128),
                           ("viaAlias(uint256,uint256)", 128)):
            assert call(sig, key, index) == (0,)
        for sig in ("nested(uint256,uint256,uint256)", "readNested(uint256,uint256,uint256)"):
            assert call(sig, key, 1, 128) == (0,)
        for sig in ("pairs(uint256,uint256)", "readPair(uint256,uint256)", "copyPair(uint256,uint256)",
                    "assignPair(uint256,uint256)", "argumentPair(uint256,uint256)", "returnPair(uint256,uint256)"):
            assert call(sig, key, 256, returns=pair_types) == empty_pair
    assert _boxes(harness, app) == boxes
    assert call("effectful(uint256,uint256)", 7, 128, returns=("uint256",) * 3) == (0, 1, 1)
    assert _boxes(harness, app) == boxes

    for sig in ("above(uint256,uint256)", "readAbove(uint256,uint256)", "viaAlias(uint256,uint256)"):
        for index in (129, 2**64, 2**200):
            call(sig, 7, index, revert=True)
    for sig in ("nested(uint256,uint256,uint256)", "readNested(uint256,uint256,uint256)"):
        for i, j in ((2, 0), (0, 129), (1, 2**200)):
            call(sig, 7, i, j, revert=True)
    assert _boxes(harness, app) == boxes

    call("addAbove(uint256,uint256,uint256)", 7, 128, 11, returns=())
    call("setEdge(uint256,uint256,uint256)", 7, 127, 22, returns=())
    call("setCap(uint256,uint256,uint256)", 7, 1023, 33, returns=())
    call("setNested(uint256,uint256,uint256,uint256)", 7, 1, 128, 44, returns=())
    call("setPair(uint256,uint256)", 7, 256, returns=())
    written_boxes = _boxes(harness, app)
    assert call("above(uint256,uint256)", 7, 128) == (11,)
    assert call("viaAlias(uint256,uint256)", 7, 128) == (11,)
    assert call("edge(uint256,uint256)", 7, 127) == (22,)
    assert call("cap(uint256,uint256)", 7, 1023) == (33,)
    assert call("nested(uint256,uint256,uint256)", 7, 1, 128) == (44,)
    for sig in ("pairs(uint256,uint256)", "readPair(uint256,uint256)", "copyPair(uint256,uint256)",
                "assignPair(uint256,uint256)", "argumentPair(uint256,uint256)", "returnPair(uint256,uint256)"):
        assert call(sig, 7, 256, returns=pair_types) == (-32769, 65535, 1, bytes.fromhex("123456"))
        assert call(sig, 9, 256, returns=pair_types) == empty_pair
    assert call("effectful(uint256,uint256)", 7, 128, returns=("uint256",) * 3) == (11, 2, 2)
    assert _boxes(harness, app) == written_boxes
    # An inner index cannot reach the next row, even though that byte offset
    # would still be inside the enclosing box. Writes must also revert.
    call("setNested(uint256,uint256,uint256,uint256)", 7, 0, 129, 99, returns=(), revert=True)
    assert call("nested(uint256,uint256,uint256)", 7, 1, 0) == (0,)

    call("clear(uint256)", 7, returns=())
    cleared_boxes = _boxes(harness, app)
    assert call("readAbove(uint256,uint256)", 7, 128) == (0,)
    assert call("nested(uint256,uint256,uint256)", 7, 1, 128) == (0,)
    assert call("pairs(uint256,uint256)", 7, 256, returns=pair_types) == empty_pair
    assert _boxes(harness, app) == cleared_boxes
    call("setAbove(uint256,uint256,uint256)", 7, 128, 55, returns=())
    assert call("above(uint256,uint256)", 7, 128) == (55,)
    assert call("above(uint256,uint256)", 9, 128) == (0,)


@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_lazy_fixed_struct_projections(harness, abi, via_yul_behavior):
    app = harness.compile_and_deploy("puyasolRegression/contracts/rev_2_lazy_struct_box.sol",
                                     extra_args=["--contract-abi", abi], fund_wei=5_000_000,
                                     via_yul_behavior=via_yul_behavior)
    boxes = _boxes(harness, app)
    assert _call(harness, app, abi, "roots(uint256)", (7,), ("uint256", "uint16")) == (0, 0)
    assert _call(harness, app, abi, "read(uint256)", (7,), ("uint256", "uint256", "uint16")) == (0, 0, 0)
    assert _boxes(harness, app) == boxes
    _call(harness, app, abi, "set(uint256)", (7,))
    assert _call(harness, app, abi, "read(uint256)", (7,), ("uint256", "uint256", "uint16")) == (11, 22, 33)
    _call(harness, app, abi, "clear(uint256)", (7,))
    boxes = _boxes(harness, app)
    assert _call(harness, app, abi, "read(uint256)", (7,), ("uint256", "uint256", "uint16")) == (0, 0, 0)
    assert _boxes(harness, app) == boxes


@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_deleted_pages_default_and_recreate_exact_size(harness, abi, via_yul_behavior):
    app = harness.compile_and_deploy("puyasolRegression/contracts/rev_2_lazy_pages.sol",
                                     extra_args=["--contract-abi", abi], fund_wei=30_000_000,
                                     postinit_budget_pool=14, via_yul_behavior=via_yul_behavior)
    def call(sig, *args, returns=("uint256",), revert=False):
        return _call(harness, app, abi, sig, args, returns, revert)

    call("clear()", returns=())
    boxes = _boxes(harness, app)
    for sig in ("pages(uint256)", "read(uint256)"):
        for index in (0, 1023, 1024):
            assert call(sig, index) == (0,)
        for index in (1025, 2**64, 2**200):
            call(sig, index, revert=True)
    call("clearElement(uint256)", 1024, returns=())
    assert _boxes(harness, app) == boxes
    call("add(uint256,uint256)", 1024, 11, returns=())
    new_boxes = _boxes(harness, app) - boxes
    assert len(new_boxes) == 1
    page = harness.localnet.algod.application_box_by_name(app.app_id, base64.b64decode(next(iter(new_boxes))))
    assert len(base64.b64decode(page["value"])) == 32
    call("set(uint256,uint256)", 1023, 22, returns=())
    assert call("pages(uint256)", 1024) == (11,)
    assert call("read(uint256)", 1023) == (22,)
    assert call("read(uint256)", 0) == (0,)
    call("clear()", returns=())
    boxes = _boxes(harness, app)
    assert call("read(uint256)", 1023) == (0,)
    assert call("pages(uint256)", 1024) == (0,)
    assert _boxes(harness, app) == boxes
    call("set(uint256,uint256)", 1024, 33, returns=())
    assert call("read(uint256)", 1024) == (33,)


@pytest.mark.parametrize("fixture", ["mapping_pages", "page_alias"])
def test_unsupported_lazy_paging_is_diagnosed(harness, fixture):
    with pytest.raises(CompileError, match="multi-box storage access requires a supported paged declaration path"):
        harness.compile(f"puyasolRegression/contracts/rev_2_lazy_unsupported_{fixture}.sol")


def test_slot_mode_retains_explicit_whole_array_delete_capacity(harness):
    with pytest.raises(CompileError, match="delete on storage array of length 129 not supported \\(cap 64\\)"):
        harness.compile("puyasolRegression/contracts/rev_2_lazy_fixed_boxes.sol", extra_args=["--evm-storage-layout"])


def test_interior_large_array_reference_is_not_a_whole_box(harness):
    with pytest.raises(CompileError, match="large fixed-array storage references require a whole-box root"):
        harness.compile("puyasolRegression/contracts/rev_2_lazy_unsupported_interior_ref.sol")
