"""Tests for the revertStrings category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


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

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_array_dynamic_invalid(harness):
    """revertStrings/contracts/calldata_array_dynamic_invalid.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_array_dynamic_static_short_decode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_decode.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_array_dynamic_static_short_reencode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_reencode.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_array_invalid_length(harness):
    """revertStrings/contracts/calldata_array_invalid_length.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_arrays_too_large(harness):
    """revertStrings/contracts/calldata_arrays_too_large.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_tail_short(harness):
    """revertStrings/contracts/calldata_tail_short.sol"""

@pytest.mark.skip(reason="EVM-flat calldata corruption test; ARC4 encoding is structurally different")
def test_calldata_too_short_v1(harness):
    """revertStrings/contracts/calldata_too_short_v1.sol"""

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

@pytest.mark.skip(reason="EVM dispatcher Calldata-too-short check; ARC4 has no equivalent")
def test_function_entry_checks_v1(harness):
    """revertStrings/contracts/function_entry_checks_v1.sol"""

@pytest.mark.skip(reason="EVM dispatcher Calldata-too-short check; ARC4 has no equivalent")
def test_function_entry_checks_v2(harness):
    """revertStrings/contracts/function_entry_checks_v2.sol"""

@pytest.mark.skip(reason="EVM calldata head/data-pointer corruption test; N/A on ARC4")
def test_invalid_abi_decoding_calldata_v1(harness):
    """revertStrings/contracts/invalid_abi_decoding_calldata_v1.sol"""

@pytest.mark.skip(reason="EVM memory abi-decode corruption test; AVM has no analogous memory layout")
def test_invalid_abi_decoding_memory_v1(harness):
    """revertStrings/contracts/invalid_abi_decoding_memory_v1.sol"""

@pytest.mark.skip(reason="EVM-style address(L).call(...) returning encoded revert debug bytes; AVM inlines libs")
def test_library_non_view_call(harness):
    """revertStrings/contracts/library_non_view_call.sol"""

@pytest.mark.skip(reason="EVM calldata stride/length check; ARC4 encoding eliminates this class of error")
def test_short_input_array(harness):
    """revertStrings/contracts/short_input_array.sol"""

@pytest.mark.skip(reason="EVM calldata stride/length check; ARC4 encoding eliminates this class of error")
def test_short_input_bytes(harness):
    """revertStrings/contracts/short_input_bytes.sol"""

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
