"""Tests for the errors category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_error_in_library_and_interface(harness):
    """errors/contracts/error_in_library_and_interface.sol"""
    app = harness.compile_and_deploy("errors/contracts/error_in_library_and_interface.sol")
    # f() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"85208890", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000002"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE, hex"7924ea7c", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000002", hex"0000000000000000000000000000000000000000000000000000000000000003"
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_error_selector(harness):
    """errors/contracts/error_selector.sol
    SELECTOR NOTE: error selectors follow the project-wide sha512_256
    convention (selector == revert-payload prefix == sha512_256 of the
    canonical no-return signature). EVM uses keccak256 — EVM_DIVERGENCE.
    """
    app = harness.compile_and_deploy("errors/contracts/error_selector.sol")
    # EVM_DIVERGENCE: error selectors are sha512_256(canonicalSig)[:4],
    # matching the custom-error revert payload prefix (EVM uses keccak).
    from framework import arc4_selector
    e1 = arc4_selector("E()")
    e2 = arc4_selector("E(uint256)")
    sel_F = arc4_selector("F()")
    r = harness.call(app, "test1()")
    assert [bytes(x) for x in r.abi_return] == [e1, e2, e1, e1]
    r = harness.call(app, "test2()")
    assert [bytes(x) for x in r.abi_return] == [e1, e2, e1, e1]
    assert bytes(harness.call(app, "test3()").abi_return) == sel_F

def test_error_static_calldata_uint_array_and_dynamic_array(harness):
    """errors/contracts/error_static_calldata_uint_array_and_dynamic_array.sol"""
    app = harness.compile_and_deploy("errors/contracts/error_static_calldata_uint_array_and_dynamic_array.sol")
    r = harness.call(app, "f(uint256[],uint256[1])", [255], [65535], expect_revert=True)
    assert r.reverted

def test_error_throw_from_module_via_member_access(harness):
    """errors/contracts/error_throw_from_module_via_member_access.sol"""
    app = harness.compile_and_deploy("errors/contracts/error_throw_from_module_via_member_access.sol")
    # error1() -> FAILURE, hex"a5f9ec67", 0x20, 7, "B error"
    r = harness.call(app, "error1()", expect_revert=True)
    assert r.reverted
    # error2() -> FAILURE, hex"a5f9ec67", 0x20, 17, "B.BContract error"
    r = harness.call(app, "error2()", expect_revert=True)
    assert r.reverted
    # error3() -> FAILURE, hex"23b0db14", 0x20, 9, "B.A error"
    r = harness.call(app, "error3()", expect_revert=True)
    assert r.reverted

def test_errors_by_parameter_type(harness):
    """errors/contracts/errors_by_parameter_type.sol"""
    app = harness.compile_and_deploy("errors/contracts/errors_by_parameter_type.sol", via_yul_behavior=True)
    # a() -> FAILURE, hex"92bbf6e8"
    r = harness.call(app, "a()", expect_revert=True)
    assert r.reverted
    # b() -> FAILURE, hex"47e26897", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "b()", expect_revert=True)
    assert r.reverted
    # c() -> FAILURE, hex"8f372c34", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"000000000000000000000000000000000000000000000000000000000000000e", hex"737472696e67206c69746572616c000000000000000000000000000000000000"
    r = harness.call(app, "c()", expect_revert=True)
    assert r.reverted
    # d() -> FAILURE, hex"5717173e", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"000000000000000000000000000000000000000000000000000000000000000e", hex"737472696e67206c69746572616c000000000000000000000000000000000000"
    r = harness.call(app, "d()", expect_revert=True)
    assert r.reverted
    # e() -> FAILURE, hex"7efef9ea", hex"0000000000000000000000000000000000000000000000000000000000001234"
    r = harness.call(app, "e()", expect_revert=True)
    assert r.reverted
    # f() -> FAILURE, hex"0c3f12eb", hex"00000000000000000000000000000000000012340dbe671f0000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_named_error_args(harness):
    """errors/contracts/named_error_args.sol"""
    app = harness.compile_and_deploy("errors/contracts/named_error_args.sol")
    # f() -> FAILURE, hex"85208890", hex"0000000000000000000000000000000000000000000000000000000000000002", hex"0000000000000000000000000000000000000000000000000000000000000007"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_named_parameters_shadowing_types(harness):
    """errors/contracts/named_parameters_shadowing_types.sol"""
    app = harness.compile_and_deploy("errors/contracts/named_parameters_shadowing_types.sol")
    # f() -> FAILURE, hex"33a54193", hex"000000000000000000000000000000000000000000000000000000000000002a"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"374b9387", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"000000000000000000000000000000000000000000000000000000000000002a"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_panic_via_import(harness):
    """errors/contracts/panic_via_import.sol"""
    app = harness.compile_and_deploy("errors/contracts/panic_via_import.sol")
    # a() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "a()", expect_revert=True)
    assert r.reverted
    # b() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "b()", expect_revert=True)
    assert r.reverted

def test_require_different_errors_same_parameters(harness):
    """errors/contracts/require_different_errors_same_parameters.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_different_errors_same_parameters.sol")
    # f() -> FAILURE, hex"f55fefe3", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"74776f0000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"44a06798", hex"0000000000000000000000000000000000000000000000000000000000000004", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000006", hex"0000000000000000000000000000000000000000000000000000000000000004", hex"6669766500000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_require_error_condition_evaluated_only_once(harness):
    """errors/contracts/require_error_condition_evaluated_only_once.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_condition_evaluated_only_once.sol")
    # f(bool): false -> FAILURE, hex"110b3655", 1
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # getCounter() -> 0
    r = harness.call(app, "getCounter()")
    assert as_int(r.abi_return) == 0
    # f(bool): true ->
    r = harness.call(app, "f(bool)", True)
    # (void return — call succeeding is the assertion)
    # getCounter() -> 1
    r = harness.call(app, "getCounter()")
    assert as_int(r.abi_return) == 1

def test_require_error_evaluation_order_1(harness):  # currently fails
    """errors/contracts/require_error_evaluation_order_1.sol"""
    app = harness.compile_and_deploy('errors/contracts/require_error_evaluation_order_1.sol')
    r = harness.call(app, 'f()')
    assert as_int(r.abi_return) == 7
    r = harness.call(app, 'g()')
    assert as_int(r.abi_return) == 7

def test_require_error_evaluation_order_2(harness):
    """errors/contracts/require_error_evaluation_order_2.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_evaluation_order_2.sol")
    # f(bool): false -> FAILURE, hex"002ff067", 42
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # f(bool): true ->
    r = harness.call(app, "f(bool)", True)
    # (void return — call succeeding is the assertion)

def test_require_error_evaluation_order_3(harness):
    """errors/contracts/require_error_evaluation_order_3.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_evaluation_order_3.sol")
    # f(bool): false -> FAILURE, hex"08c379a0", 0x20, 0x1b, "Intercepted failure message"
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted
    # f(bool): true ->
    r = harness.call(app, "f(bool)", True)
    # (void return — call succeeding is the assertion)

def test_require_error_function_join_control_flow(harness):
    """errors/contracts/require_error_function_join_control_flow.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_function_join_control_flow.sol")
    # f(bool): true -> 0x15, 0x15, 0
    r = harness.call(app, "f(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (21, 21, 0)
    # f(bool): false -> FAILURE, hex"002ff067", 21
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted

def test_require_error_function_pointer_parameter(harness):
    """errors/contracts/require_error_function_pointer_parameter.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_function_pointer_parameter.sol")
    # f() -> FAILURE, hex"271b1dfa", hex"0000000000000000000000000000000000001234f37cdc8e0000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_require_error_multiple_arguments(harness):
    """errors/contracts/require_error_multiple_arguments.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_multiple_arguments.sol")
    # f() -> FAILURE, hex"11a1077e", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"74776f0000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"11a1077e", hex"0000000000000000000000000000000000000000000000000000000000000004", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000006", hex"0000000000000000000000000000000000000000000000000000000000000004", hex"6669766500000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_require_error_stack_check(harness):
    """errors/contracts/require_error_stack_check.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_stack_check.sol")
    # f(bool,uint256,uint256,uint256): true, 42, 4242, 424242 ->
    r = harness.call(app, "f(bool,uint256,uint256,uint256)", True, 42, 4242, 424242)
    # (void return — call succeeding is the assertion)
    # x() -> 4242
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 4242

def test_require_error_string_literal(harness):
    """errors/contracts/require_error_string_literal.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_string_literal.sol")
    # f() -> FAILURE, hex"8d6ea8be", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"000000000000000000000000000000000000000000000000000000000000000b", hex"6572726f72526561736f6e000000000000000000000000000000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"8d6ea8be", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"000000000000000000000000000000000000000000000000000000000000000d", hex"616e6f74686572526561736f6e00000000000000000000000000000000000000"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_require_error_string_memory(harness):
    """errors/contracts/require_error_string_memory.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_string_memory.sol")
    # f() -> FAILURE, hex"8d6ea8be", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"000000000000000000000000000000000000000000000000000000000000000b", hex"6572726f72526561736f6e000000000000000000000000000000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"8d6ea8be", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"000000000000000000000000000000000000000000000000000000000000000d", hex"616e6f74686572526561736f6e00000000000000000000000000000000000000"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_require_error_uint256(harness):
    """errors/contracts/require_error_uint256.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_error_uint256.sol")
    # f() -> FAILURE, hex"110b3655", 1
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"110b3655", 2
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_require_inherited_error(harness):
    """errors/contracts/require_inherited_error.sol"""
    app = harness.compile_and_deploy("errors/contracts/require_inherited_error.sol")
    # f() -> FAILURE, hex"11a1077e", hex"0000000000000000000000000000000000000000000000000000000000000001", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"74776f0000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_revert_conversion(harness):
    """errors/contracts/revert_conversion.sol"""
    app = harness.compile_and_deploy("errors/contracts/revert_conversion.sol")
    # f() -> FAILURE, hex"59e4d4df", 0x40, 0x80, 3, "abc", 1, 7
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_simple(harness):
    """errors/contracts/simple.sol"""
    app = harness.compile_and_deploy("errors/contracts/simple.sol")
    # f() -> FAILURE, hex"85208890", 2, 7
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_small_error_optimization(harness):  # currently fails
    """errors/contracts/small_error_optimization.sol"""
    app = harness.compile_and_deploy('errors/contracts/small_error_optimization.sol')
    r = harness.call(app, 'f()', expect_revert=True)
    assert r.reverted

def test_using_structs(harness):
    """errors/contracts/using_structs.sol"""
    app = harness.compile_and_deploy("errors/contracts/using_structs.sol")
    # f(bool): true -> FAILURE, hex"e96e07f0", hex"0000000000000000000000000000000000000000000000000000000000000002", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000007", hex"0000000000000000000000000000000000000000000000000000000000000009", hex"0000000000000000000000000000000000000000000000000000000000000040", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"6162630000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "f(bool)", True, expect_revert=True)
    assert r.reverted
    # f(bool): false -> FAILURE, hex"e96e07f0", hex"0000000000000000000000000000000000000000000000000000000000000002", hex"0000000000000000000000000000000000000000000000000000000000000060", hex"0000000000000000000000000000000000000000000000000000000000000007", hex"0000000000000000000000000000000000000000000000000000000000000009", hex"0000000000000000000000000000000000000000000000000000000000000040", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"6162630000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "f(bool)", False, expect_revert=True)
    assert r.reverted

def test_via_contract_type(harness):
    """errors/contracts/via_contract_type.sol"""
    app = harness.compile_and_deploy("errors/contracts/via_contract_type.sol")
    # f() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # h() -> FAILURE, hex"3e9992c9", hex"0000000000000000000000000000000000000000000000000000000000000020", hex"0000000000000000000000000000000000000000000000000000000000000003", hex"6162630000000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted

def test_via_import(harness):
    """errors/contracts/via_import.sol"""
    app = harness.compile_and_deploy("errors/contracts/via_import.sol")
    # x() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000001"
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # y() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000002"
    r = harness.call(app, "y()", expect_revert=True)
    assert r.reverted
    # z() -> FAILURE, hex"002ff067", hex"0000000000000000000000000000000000000000000000000000000000000003"
    r = harness.call(app, "z()", expect_revert=True)
    assert r.reverted

def test_weird_name(harness):
    """errors/contracts/weird_name.sol"""
    app = harness.compile_and_deploy("errors/contracts/weird_name.sol")
    # f() -> FAILURE, hex"b48fb6cf", hex"0000000000000000000000000000000000000000000000000000000000000002"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted


def test_custom_error_payload(harness):
    """errors/contracts/custom_error_payload.sol

    CUSTOM regression guard (NOT vendored). `revert E(args)` and
    `require(cond, E(args))` log the custom-error payload —
    sha512_256(canonicalSig)[:4] ++ ARC4(args) — before failing.
    EVM_DIVERGENCE: BOTH the selector (sha512_256, same hashing as ARC-28 events
    and ARC-4 methods — matching abi.encodeCall's deliberate divergence, NOT EVM
    keccak) AND the args (ARC4, coerced to the error's declared param types, NOT
    EVM head/tail) follow the AVM convention; only the fixed Error(string)/Panic
    constants stay EVM-literal. The success path evaluates the error args eagerly
    (Solidity semantics) but logs nothing.
    """
    import hashlib
    from framework import arc4_encode

    def sel(sig: str) -> bytes:
        return hashlib.new("sha512_256", sig.encode()).digest()[:4]

    app = harness.compile_and_deploy("errors/contracts/custom_error_payload.sol")
    r = harness.call(app, "p()", expect_revert=True)
    assert r.revert_data == sel("Plain()"), r.revert_data.hex()
    r = harness.call(app, "w()", expect_revert=True)
    assert r.revert_data == sel("WithArgs(uint256,string)") + arc4_encode("(uint256,string)", [7, "xy"])
    r = harness.call(app, "r(bool)", False, expect_revert=True)
    assert r.revert_data == sel("WithArgs(uint256,string)") + arc4_encode("(uint256,string)", [9, "zz"])
    r = harness.call(app, "rOk()")
    assert not r.reverted and as_int(r.abi_return) == 5
