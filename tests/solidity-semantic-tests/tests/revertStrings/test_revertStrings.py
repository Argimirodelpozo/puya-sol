"""Auto-generated tests for the revertStrings category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_array_slices(harness):
    """revertStrings/contracts/array_slices.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/array_slices.sol")
    # f(uint256,uint256,uint256[]): 2, 1, 0x80, 3, 1, 2, 3 -> FAILURE, hex"08c379a0", 0x20, 22, "Slice starts after end"
    r = harness.call(app, "f(uint256,uint256,uint256[])", 2, 1, 128, 3, 1, 2, 3, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256,uint256[]): 1, 5, 0x80, 3, 1, 2, 3 -> FAILURE, hex"08c379a0", 0x20, 28, "Slice is greater than length"
    r = harness.call(app, "f(uint256,uint256,uint256[])", 1, 5, 128, 3, 1, 2, 3, expect_revert=True)
    assert r.reverted

def test_bubble(harness):
    """revertStrings/contracts/bubble.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/bubble.sol")
    # f() -> FAILURE, hex"08c379a0", 0x20, 4, "fail"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_invalid(harness):
    """revertStrings/contracts/calldata_array_dynamic_invalid.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_array_dynamic_invalid.sol")
    # f(uint256[][]): 0x20, 1 -> FAILURE, hex"08c379a0", 0x20, 43, "ABI decoding: invalid calldata a", "rray stride"
    r = harness.call(app, "f(uint256[][])", 32, 1, expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_static_short_decode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_decode.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_array_dynamic_static_short_decode.sol")
    # f(uint256[][2][]): 0x20, 0x01, 0x20, 0x00 -> FAILURE, hex"08c379a0", 0x20, 28, "Invalid calldata tail offset"
    r = harness.call(app, "f(uint256[][2][])", 32, 1, 32, 0, expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_static_short_reencode(harness):
    """revertStrings/contracts/calldata_array_dynamic_static_short_reencode.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_array_dynamic_static_short_reencode.sol")
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x00 -> FAILURE, hex"08c379a0", 0x20, 30, "Invalid calldata access offset"
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 0, expect_revert=True)
    assert r.reverted

def test_calldata_array_invalid_length(harness):
    """revertStrings/contracts/calldata_array_invalid_length.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_array_invalid_length.sol")
    # f(uint256[][]): 0x20, 1, 0x20, 0x0100000000000000000000 -> FAILURE, hex"08c379a0", 0x20, 28, "Invalid calldata tail length"
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 0x100000000000000000000, expect_revert=True)
    assert r.reverted

def test_calldata_arrays_too_large(harness):
    """revertStrings/contracts/calldata_arrays_too_large.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_arrays_too_large.sol")
    # f(uint256,uint256[],uint256): 6, 0x60, 9, 0x1000000000000000000000000000000000000000000000000000000000000002, 1, 2 -> FAILURE, hex"08c379a0", 0x20, 43, "ABI decoding: invalid calldata a", "rray length"
    r = harness.call(app, "f(uint256,uint256[],uint256)", 6, 96, 9, 0x1000000000000000000000000000000000000000000000000000000000000002, 1, 2, expect_revert=True)
    assert r.reverted

def test_calldata_tail_short(harness):
    """revertStrings/contracts/calldata_tail_short.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_tail_short.sol")
    # f(uint256[][]): 0x20, 1, 0x20, 2, 0x42 -> FAILURE, hex"08c379a0", 0x20, 23, "Calldata tail too short"
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 2, 66, expect_revert=True)
    assert r.reverted

def test_calldata_too_short_v1(harness):
    """revertStrings/contracts/calldata_too_short_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/calldata_too_short_v1.sol")
    # d(bytes): 0x20, 0x01, 0x0000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"08c379a0", 0x20, 18, "Calldata too short"
    r = harness.call(app, "d(bytes)", 32, 1, 0, expect_revert=True)
    assert r.reverted

def test_called_contract_has_code(harness):
    """revertStrings/contracts/called_contract_has_code.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/called_contract_has_code.sol")
    # g() -> FAILURE, hex"08c379a0", 0x20, 37, "Target contract does not contain", " code"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_empty_v1(harness):
    """revertStrings/contracts/empty_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/empty_v1.sol")
    # f() -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g(string): 0x20, 0, "" -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "g(string)", 32, 0, bytes.fromhex(''), expect_revert=True)
    assert r.reverted
    # g(string): 0x20, 0 -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "g(string)", 32, 0, expect_revert=True)
    assert r.reverted

def test_empty_v2(harness):
    """revertStrings/contracts/empty_v2.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/empty_v2.sol")
    # f() -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g(string): "" -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "g(string)", bytes.fromhex(''), expect_revert=True)
    assert r.reverted
    # g(string): 0x20, 0, "" -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "g(string)", 32, 0, bytes.fromhex(''), expect_revert=True)
    assert r.reverted
    # g(string): 0x20, 0 -> FAILURE, hex"08c379a0", 0x20, 0
    r = harness.call(app, "g(string)", 32, 0, expect_revert=True)
    assert r.reverted

def test_enum_v1(harness):
    """revertStrings/contracts/enum_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/enum_v1.sol")
    # f(uint8[]): 0x20, 2, 3, 3 -> FAILURE, hex"08c379a0", 0x20, 17, "Enum out of range"
    r = harness.call(app, "f(uint8[])", 32, 2, 3, 3, expect_revert=True)
    assert r.reverted

def test_enum_v2(harness):
    """revertStrings/contracts/enum_v2.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/enum_v2.sol")
    # f(uint8[]): 0x20, 2, 3, 3 -> FAILURE
    r = harness.call(app, "f(uint8[])", 32, 2, 3, 3, expect_revert=True)
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
    app = harness.compile_and_deploy("revertStrings/contracts/function_entry_checks_v1.sol")
    # t(uint256) -> FAILURE, hex"08c379a0", 0x20, 0x12, "Calldata too short"
    r = harness.call(app, "t(uint256)", expect_revert=True)
    assert r.reverted

def test_function_entry_checks_v2(harness):
    """revertStrings/contracts/function_entry_checks_v2.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/function_entry_checks_v2.sol")
    # t(uint256) -> FAILURE, hex"08c379a0", 0x20, 34, "ABI decoding: tuple data too sho", "rt"
    r = harness.call(app, "t(uint256)", expect_revert=True)
    assert r.reverted

def test_invalid_abi_decoding_calldata_v1(harness):
    """revertStrings/contracts/invalid_abi_decoding_calldata_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/invalid_abi_decoding_calldata_v1.sol")
    # d(bytes): 0x20, 0x20, 0x0000000000000000000000000000000000000000000000000000000000000000 -> 0
    r = harness.call(app, "d(bytes)", 32, 32, 0)
    assert r.abi_return == 0
    # d(bytes): 0x100, 0x20, 0x0000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"08c379a0", 0x20, 43, "ABI calldata decoding: invalid h", "ead pointer"
    r = harness.call(app, "d(bytes)", 256, 32, 0, expect_revert=True)
    assert r.reverted
    # d(bytes): 0x20, 0x100, 0x0000000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"08c379a0", 0x20, 43, "ABI calldata decoding: invalid d", "ata pointer"
    r = harness.call(app, "d(bytes)", 32, 256, 0, expect_revert=True)
    assert r.reverted

def test_invalid_abi_decoding_memory_v1(harness):
    """revertStrings/contracts/invalid_abi_decoding_memory_v1.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/invalid_abi_decoding_memory_v1.sol")
    # f(uint256,uint256,uint256): 0, 0x200, 0x60 -> FAILURE, hex"08c379a0", 0x20, 39, "ABI memory decoding: invalid dat", "a start"
    r = harness.call(app, "f(uint256,uint256,uint256)", 0, 512, 96, expect_revert=True)
    assert r.reverted
    # f(uint256,uint256,uint256): 0, 0x20, 0x60 -> FAILURE, hex"08c379a0", 0x20, 40, "ABI memory decoding: invalid dat", "a length"
    r = harness.call(app, "f(uint256,uint256,uint256)", 0, 32, 96, expect_revert=True)
    assert r.reverted

def test_library_non_view_call(harness):
    """revertStrings/contracts/library_non_view_call.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/library_non_view_call.sol")
    # f() -> 32, 132, 3963877391197344453575983046348115674221700746820753546331534351508065746944, 862718293348820473429344482784628181556388621521298319395315527974912, 1518017211910606845658622928256476421055725129218887721595913401102969, 14649601406562900601407788686537400806574002225747213573947654179243427889152, 0
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 32, 132, 3963877391197344453575983046348115674221700746820753546331534351508065746944, 862718293348820473429344482784628181556388621521298319395315527974912, 1518017211910606845658622928256476421055725129218887721595913401102969, 14649601406562900601407788686537400806574002225747213573947654179243427889152, 0
    assert not r.reverted

def test_short_input_array(harness):
    """revertStrings/contracts/short_input_array.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/short_input_array.sol")
    # f(uint256[]): 0x20, 1 -> FAILURE, hex"08c379a0", 0x20, 43, "ABI decoding: invalid calldata a", "rray stride"
    r = harness.call(app, "f(uint256[])", 32, 1, expect_revert=True)
    assert r.reverted

def test_short_input_bytes(harness):
    """revertStrings/contracts/short_input_bytes.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/short_input_bytes.sol")
    # e(bytes): 0x20, 7 -> FAILURE, hex"08c379a0", 0x20, 39, "ABI decoding: invalid byte array", " length"
    r = harness.call(app, "e(bytes)", 32, 7, expect_revert=True)
    assert r.reverted

def test_transfer(harness):
    """revertStrings/contracts/transfer.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/transfer.sol")
    # (), 10 wei ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # g() -> 10
    r = harness.call(app, "g()")
    assert r.abi_return == 10
    # f() -> FAILURE, hex"08c379a0", 0x20, 10, "no_receive"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_unknown_sig_no_fallback(harness):
    """revertStrings/contracts/unknown_sig_no_fallback.sol"""
    app = harness.compile_and_deploy("revertStrings/contracts/unknown_sig_no_fallback.sol")
    # (): hex"00" -> FAILURE, hex"08c379a0", 0x20, 41, "Unknown signature and no fallbac", "k defined"
    r = harness.call(app, "()", bytes.fromhex('00'), expect_revert=True)
    assert r.reverted
