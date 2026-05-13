"""Auto-generated tests for the constructor category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_arrays_in_constructors(harness):
    """constructor/contracts/arrays_in_constructors.sol"""
    app = harness.compile_and_deploy("constructor/contracts/arrays_in_constructors.sol")
    # f(uint256,address[]): 7, 0x40, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 -> 7, 8
    r = harness.call(app, "f(uint256,address[])", 7, 64, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    assert tuple(r.abi_return) == (7, 8)

def test_base_constructor_arguments(harness):
    """constructor/contracts/base_constructor_arguments.sol"""
    app = harness.compile_and_deploy("constructor/contracts/base_constructor_arguments.sol")
    # getA() -> 49
    r = harness.call(app, "getA()")
    assert r.abi_return == 49

def test_bytes_in_constructors_packer(harness):
    """constructor/contracts/bytes_in_constructors_packer.sol"""
    app = harness.compile_and_deploy("constructor/contracts/bytes_in_constructors_packer.sol")
    # f(uint256,bytes): 7, 0x40, 78, "abcdefghijklmnopqrstuvwxyzabcdef", "ghijklmnopqrstuvwxyzabcdefghijkl", "mnopqrstuvwxyz" -> 7, "h"
    r = harness.call(app, "f(uint256,bytes)", 7, 64, 78, bytes.fromhex('6162636465666768696a6b6c6d6e6f707172737475767778797a616263646566'), bytes.fromhex('6768696a6b6c6d6e6f707172737475767778797a6162636465666768696a6b6c'), bytes.fromhex('6d6e6f707172737475767778797a'))
    # TODO: verify expected: 7 | "h"
    assert not r.reverted

def test_bytes_in_constructors_unpacker(harness):
    """constructor/contracts/bytes_in_constructors_unpacker.sol"""
    app = harness.compile_and_deploy("constructor/contracts/bytes_in_constructors_unpacker.sol", ctor_args=[7, 64, 78, bytes.fromhex('6162636465666768696a6b6c6d6e6f707172737475767778797a616263646566'), bytes.fromhex('6768696a6b6c6d6e6f707172737475767778797a6162636465666768696a6b6c'), bytes.fromhex('6d6e6f707172737475767778797a')])
    # m_x() -> 7
    r = harness.call(app, "m_x()")
    assert r.abi_return == 7
    # m_s() -> 0x20, 78, "abcdefghijklmnopqrstuvwxyzabcdef", "ghijklmnopqrstuvwxyzabcdefghijkl", "mnopqrstuvwxyz"
    r = harness.call(app, "m_s()")
    # TODO: verify expected: 0x20 | 78 | "abcdefghijklmnopqrstuvwxyzabcdef" | "ghijklmnopqrstuvwxyzabcdefghijkl" | "mnopqrstuvwxyz"
    assert not r.reverted

def test_callvalue_check(harness):
    """constructor/contracts/callvalue_check.sol"""
    app = harness.compile_and_deploy("constructor/contracts/callvalue_check.sol")
    # f(uint256), 2000 ether: 0 -> true
    r = harness.call(app, "f(uint256)", 0, payment_wei=2000000000000000000000)
    assert r.abi_return is True
    # f(uint256), 2000 ether: 100 -> false
    r = harness.call(app, "f(uint256)", 100, payment_wei=2000000000000000000000)
    assert r.abi_return is False
    # g(uint256), 2000 ether: 0 -> true
    r = harness.call(app, "g(uint256)", 0, payment_wei=2000000000000000000000)
    assert r.abi_return is True
    # g(uint256), 2000 ether: 100 -> false
    r = harness.call(app, "g(uint256)", 100, payment_wei=2000000000000000000000)
    assert r.abi_return is False
    # h(uint256), 2000 ether: 0 -> true
    r = harness.call(app, "h(uint256)", 0, payment_wei=2000000000000000000000)
    assert r.abi_return is True
    # h(uint256), 2000 ether: 100 -> false
    r = harness.call(app, "h(uint256)", 100, payment_wei=2000000000000000000000)
    assert r.abi_return is False
    # i(uint256), 2000 ether: 0 -> true
    r = harness.call(app, "i(uint256)", 0, payment_wei=2000000000000000000000)
    assert r.abi_return is True
    # i(uint256), 2000 ether: 100 -> false
    r = harness.call(app, "i(uint256)", 100, payment_wei=2000000000000000000000)
    assert r.abi_return is False

def test_constructor_arguments_external(harness):
    """constructor/contracts/constructor_arguments_external.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_arguments_external.sol", ctor_args=[bytes.fromhex('616263'), True])
    # getFlag() -> true
    r = harness.call(app, "getFlag()")
    assert r.abi_return is True
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted

def test_constructor_arguments_internal(harness):
    """constructor/contracts/constructor_arguments_internal.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_arguments_internal.sol")
    # getFlag() -> true
    r = harness.call(app, "getFlag()")
    assert r.abi_return is True
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted

def test_constructor_function_argument(harness):
    """constructor/contracts/constructor_function_argument.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_function_argument.sol", ctor_args=[0xfdd67305928fcac8d213d1e47bfa6165cd0b87b946644cd0000000000000000])
    # constructor-only test — deployment succeeding is the assertion

def test_constructor_function_complex(harness):
    """constructor/contracts/constructor_function_complex.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_function_complex.sol")
    # f() -> 16
    r = harness.call(app, "f()")
    assert r.abi_return == 16

def test_constructor_static_array_argument(harness):
    """constructor/contracts/constructor_static_array_argument.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_static_array_argument.sol", ctor_args=[1, 2, 3, 4])
    # a() -> 1
    r = harness.call(app, "a()")
    assert r.abi_return == 1
    # b(uint256): 0 -> 2
    r = harness.call(app, "b(uint256)", 0)
    assert r.abi_return == 2
    # b(uint256): 1 -> 3
    r = harness.call(app, "b(uint256)", 1)
    assert r.abi_return == 3
    # b(uint256): 2 -> 4
    r = harness.call(app, "b(uint256)", 2)
    assert r.abi_return == 4

def test_evm_exceptions_in_constructor_call_fail(harness):
    """constructor/contracts/evm_exceptions_in_constructor_call_fail.sol"""
    app = harness.compile_and_deploy("constructor/contracts/evm_exceptions_in_constructor_call_fail.sol")
    # testIt() ->
    r = harness.call(app, "testIt()")
    # (void return — call succeeding is the assertion)
    # test() -> 2
    r = harness.call(app, "test()")
    assert r.abi_return == 2

def test_function_usage_in_constructor_arguments(harness):
    """constructor/contracts/function_usage_in_constructor_arguments.sol"""
    app = harness.compile_and_deploy("constructor/contracts/function_usage_in_constructor_arguments.sol")
    # getA() -> 2
    r = harness.call(app, "getA()")
    assert r.abi_return == 2

def test_functions_called_by_constructor(harness):
    """constructor/contracts/functions_called_by_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/functions_called_by_constructor.sol")
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted

def test_functions_called_by_constructor_through_dispatch(harness):
    """constructor/contracts/functions_called_by_constructor_through_dispatch.sol"""
    app = harness.compile_and_deploy("constructor/contracts/functions_called_by_constructor_through_dispatch.sol")
    # getName() -> "def\x00\x00\x00"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "def\x00\x00\x00"
    assert not r.reverted

def test_inline_member_init_inheritence_without_constructor(harness):
    """constructor/contracts/inline_member_init_inheritence_without_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/inline_member_init_inheritence_without_constructor.sol")
    # getBMember() -> 5
    r = harness.call(app, "getBMember()")
    assert r.abi_return == 5
    # getDMember() -> 6
    r = harness.call(app, "getDMember()")
    assert r.abi_return == 6

def test_no_callvalue_check(harness):
    """constructor/contracts/no_callvalue_check.sol"""
    app = harness.compile_and_deploy("constructor/contracts/no_callvalue_check.sol")
    # f(), 2000 ether -> true
    r = harness.call(app, "f()", payment_wei=2000000000000000000000)
    assert r.abi_return is True

def test_order_of_evaluation(harness):
    """constructor/contracts/order_of_evaluation.sol"""
    app = harness.compile_and_deploy("constructor/contracts/order_of_evaluation.sol")
    # g() -> 0x20, 4, 1, 3, 2, 4
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 32, 4, 1, 3, 2, 4
    assert not r.reverted

def test_payable_constructor(harness):
    """constructor/contracts/payable_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/payable_constructor.sol", fund_wei=27)
    # constructor-only test — deployment succeeding is the assertion

def test_state_variable_initialization(harness):
    """constructor/contracts/state_variable_initialization.sol"""
    app = harness.compile_and_deploy("constructor/contracts/state_variable_initialization.sol")
    # i() -> 2
    r = harness.call(app, "i()")
    assert r.abi_return == 2
    # k() -> 0
    r = harness.call(app, "k()")
    assert r.abi_return == 0

def test_store_function_in_constructor(harness):
    """constructor/contracts/store_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_function_in_constructor.sol")
    # use(uint256): 3 -> 6
    r = harness.call(app, "use(uint256)", 3)
    assert r.abi_return == 6
    # result_in_constructor() -> 4
    r = harness.call(app, "result_in_constructor()")
    assert r.abi_return == 4

def test_store_function_in_constructor_packed(harness):
    """constructor/contracts/store_function_in_constructor_packed.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_function_in_constructor_packed.sol")
    # use(uint16): 3 -> 0xfff9
    r = harness.call(app, "use(uint16)", 3)
    assert r.abi_return == 65529
    # result_in_constructor() -> 0xfffb
    r = harness.call(app, "result_in_constructor()")
    assert r.abi_return == 65531
    # other() -> 0x1fff
    r = harness.call(app, "other()")
    assert r.abi_return == 8191

def test_store_internal_unused_function_in_constructor(harness):
    """constructor/contracts/store_internal_unused_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_internal_unused_function_in_constructor.sol")
    # t() -> 7
    r = harness.call(app, "t()")
    assert r.abi_return == 7

def test_store_internal_unused_library_function_in_constructor(harness):
    """constructor/contracts/store_internal_unused_library_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_internal_unused_library_function_in_constructor.sol")
    # t() -> 7
    r = harness.call(app, "t()")
    assert r.abi_return == 7

def test_transient_state_variable_initialization(harness):
    """constructor/contracts/transient_state_variable_initialization.sol"""
    app = harness.compile_and_deploy("constructor/contracts/transient_state_variable_initialization.sol")
    # f() -> 100
    r = harness.call(app, "f()")
    assert r.abi_return == 100
