"""Tests for the revertStrings category."""
import hashlib
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def _avm_sel(sig: str) -> bytes:
    """AVM ARC4 method selector — sha512_256("<name>(<args>)<ret>")[:4]."""
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def _assert_malformed_reverts(harness, sol_path: str, arc4_sig: str, bad_arg: bytes):
    """Compile+deploy `sol_path`, then call its method by AVM selector with a
    deliberately malformed ARC4 payload. Assert the call reverts.

    On EVM, these tests pass malformed *flat* calldata (e.g. offset=32,
    length=1, no data) and expect the dispatcher's pre-execution decode to
    revert with strings like "Calldata tail too short". On AVM, calldata is
    slot-shaped ARC4 — algosdk's high-level encoder won't even let us
    construct an invalid payload via the normal API, so we go through
    `call_raw` with crafted bytes. puya-sol's decode emits its own asserts
    (e.g. `extract end N is beyond length M`) on bad payloads. We assert
    "reverts" without pinning the specific message.
    """
    app = harness.compile_and_deploy(sol_path)
    sel = _avm_sel(arc4_sig)
    r = harness.call_raw(app, selector=sel, extra_args=(bad_arg,), expect_revert=True)
    assert r.reverted


def test_array_slices(harness):
    """revertStrings/contracts/array_slices.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/array_slices.sol")
    # f(start=2, end=1, arr=[1,2,3]) — start > end → "Slice starts after end".
    assert harness.call(app, "f(uint256,uint256,uint256[])", 2, 1, [1, 2, 3], expect_revert=True).reverted
    # f(start=1, end=5, arr=[1,2,3]) — end > len → "Slice is greater than length".
    assert harness.call(app, "f(uint256,uint256,uint256[])", 1, 5, [1, 2, 3], expect_revert=True).reverted

def test_bubble(harness):
    """revertStrings/contracts/bubble.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/bubble.sol")
    # f() -> FAILURE, hex"08c379a0", 0x20, 4, "fail"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_invalid(harness):
    """revertStrings/contracts/calldata_array_dynamic_invalid.sol"""
    # `uint256[][]` ARC4 = u16 outer_len + outer_len × (u16 head + data).
    # Claim outer_len=1 but provide no head bytes → decode reverts.
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_array_dynamic_invalid.sol",
        "f(uint256[][])uint256", (1).to_bytes(2, "big"))

def test_calldata_array_dynamic_static_short_decode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_decode.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_array_dynamic_static_short_decode.sol",
        "f(uint256[][2][])uint256", (1).to_bytes(2, "big"))

def test_calldata_array_dynamic_static_short_reencode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_reencode.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_array_dynamic_static_short_reencode.sol",
        "f(uint256[][2][])uint256", (1).to_bytes(2, "big"))

def test_calldata_array_invalid_length(harness):
    """revertStrings/contracts/calldata_array_invalid_length.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_array_invalid_length.sol",
        "f(uint256[][])uint256", (0xffff).to_bytes(2, "big"))

def test_calldata_arrays_too_large(harness):
    """revertStrings/contracts/calldata_arrays_too_large.sol"""
    # f(uint, uint[], uint) — pass crafted args with malformed uint[] middle.
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_arrays_too_large.sol")
    sel = _avm_sel("f(uint256,uint256[],uint256)uint256")
    a = (6).to_bytes(32, "big")
    bad_b = (0xffff).to_bytes(2, "big")  # claim max u16 elements, supply none
    c = (9).to_bytes(32, "big")
    r = harness.call_raw(app, selector=sel, extra_args=(a, bad_b, c), expect_revert=True)
    assert r.reverted

def test_calldata_tail_short(harness):
    """revertStrings/contracts/calldata_tail_short.sol"""
    # Outer length 2 but only enough head bytes for 1 entry.
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_tail_short.sol",
        "f(uint256[][])void", (2).to_bytes(2, "big") + (2).to_bytes(2, "big"))

def test_calldata_too_short_v1(harness):
    """revertStrings/contracts/calldata_too_short_v1.sol"""
    # d(bytes) — ARC4 byte[] = u16 length + N bytes. Claim 0xffff but no data.
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/calldata_too_short_v1.sol",
        "d(byte[])uint64", (0xffff).to_bytes(2, "big"))

def test_called_contract_has_code(harness):
    """revertStrings/contracts/called_contract_has_code.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/called_contract_has_code.sol")
    # g() -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_empty_v1(harness):
    """revertStrings/contracts/empty_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/empty_v1.sol")
    # f() reverts with empty string.
    assert harness.call(app, "f()", expect_revert=True).reverted
    # g("") reverts with empty string.
    assert harness.call(app, "g(string)", "", expect_revert=True).reverted

def test_empty_v2(harness):
    """revertStrings/contracts/empty_v2.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/empty_v2.sol")
    # f() and g("") both revert with empty string.
    assert harness.call(app, "f()", expect_revert=True).reverted
    assert harness.call(app, "g(string)", "", expect_revert=True).reverted

def test_enum_v1(harness):
    """revertStrings/contracts/enum_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/enum_v1.sol")
    # f([3,3]): out-of-range enum value reverts ("Enum out of range").
    r = harness.call(app, "f(uint8[])", [3, 3], expect_revert=True)
    assert r.reverted

def test_enum_v2(harness):
    """revertStrings/contracts/enum_v2.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/enum_v2.sol")
    r = harness.call(app, "f(uint8[])", [3, 3], expect_revert=True)
    assert r.reverted

def test_ether_non_payable_function(harness):
    """revertStrings/contracts/ether_non_payable_function.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/ether_non_payable_function.sol")
    # f(), 1 ether -> FAILURE, hex"08c379a0", 0x20, 34, "Ether sent to non-payable functi", "on"
    r = harness.call(app, "f()", payment_wei=1000000000000000000, expect_revert=True)
    assert r.reverted
    # () -> FAILURE, hex"08c379a0", 0x20, 53, "Contract does not have fallback ", "nor receive functions"
    r = harness.call(app, "()", expect_revert=True)
    assert r.reverted

def test_function_entry_checks_v1(harness):
    """revertStrings/contracts/function_entry_checks_v1.sol"""
    # t(uint) — pass too few bytes (uint requires 32, supply 4).
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/function_entry_checks_v1.sol",
        "t(uint256)void", b"\x00\x00\x00\x00")

def test_function_entry_checks_v2(harness):
    """revertStrings/contracts/function_entry_checks_v2.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/function_entry_checks_v2.sol",
        "t(uint256)void", b"\x00\x00\x00\x00")

def test_invalid_abi_decoding_calldata_v1(harness):
    """revertStrings/contracts/invalid_abi_decoding_calldata_v1.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/invalid_abi_decoding_calldata_v1.sol",
        "d(byte[])uint64", (0xffff).to_bytes(2, "big"))

def test_invalid_abi_decoding_memory_v1(harness):  # currently fails
    """revertStrings/contracts/invalid_abi_decoding_memory_v1.sol"""
    app = harness.compile_and_deploy('revertStrings/contracts/invalid_abi_decoding_memory_v1.sol')
    r = harness.call(app, 'f(uint256,uint256,uint256)', 0, 0x200, 0x60, expect_revert=True)
    assert r.reverted
    r = harness.call(app, 'f(uint256,uint256,uint256)', 0, 0x20, 0x60, expect_revert=True)
    assert r.reverted

def test_library_non_view_call(harness):  # currently fails
    """revertStrings/contracts/library_non_view_call.sol"""
    app = harness.compile_and_deploy('revertStrings/contracts/library_non_view_call.sol')

def test_short_input_array(harness):
    """revertStrings/contracts/short_input_array.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/short_input_array.sol",
        "f(uint256[])uint256", (1).to_bytes(2, "big"))

def test_short_input_bytes(harness):
    """revertStrings/contracts/short_input_bytes.sol"""
    _assert_malformed_reverts(
        harness, "revertStrings/contracts/short_input_bytes.sol",
        "e(byte[])uint256", (0xff).to_bytes(2, "big"))

def test_transfer(harness):
    """revertStrings/contracts/transfer.sol — bare-call w/ 10 wei lands in C.receive()."""
    app = harness.compile_and_deploy(
        "revertStrings/contracts/transfer.sol",
        postinit_inner_txns=4,
    )
    # bare call with 10 microalgos → C.receive() accepts.
    harness.call_bare(app, payment_wei=10)
    # g() returns total app balance (incl. MBR baseline) — verify the 10 microalgos arrived.
    assert as_int(harness.call(app, "g()").abi_return) - app.balance_baseline == 10
    # f() forwards 1 wei to A — A.receive() reverts("no_receive").
    assert harness.call(app, "f()", expect_revert=True).reverted
    # h() forwards 100 ether — A.receive() reverts; also overflows test runner balance.
    assert harness.call(app, "h()", expect_revert=True).reverted

def test_unknown_sig_no_fallback(harness):
    """revertStrings/contracts/unknown_sig_no_fallback.sol — unknown selector w/ no fallback reverts."""
    app = harness.compile_and_deploy("revertStrings/contracts/unknown_sig_no_fallback.sol")
    # bare-call with 1 byte of data (unknown selector) → no fallback → revert.
    r = harness.call_raw(app, b"\x00\x00\x00\x00", expect_revert=True)
    assert r.reverted
