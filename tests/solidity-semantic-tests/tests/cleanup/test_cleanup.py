"""Auto-generated tests for the cleanup category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_bool_conversion_v1(harness):
    """cleanup/contracts/bool_conversion_v1.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/bool_conversion_v1.sol")
    # f(bool): 0x0 -> 0x0
    r = harness.call(app, "f(bool)", 0)
    assert r.abi_return == 0
    # f(bool): 0x1 -> 0x1
    r = harness.call(app, "f(bool)", 1)
    assert r.abi_return == 1
    # f(bool): 0x2 -> 0x1
    r = harness.call(app, "f(bool)", 2)
    assert r.abi_return == 1
    # f(bool): 0x3 -> 0x1
    r = harness.call(app, "f(bool)", 3)
    assert r.abi_return == 1
    # f(bool): 0xff -> 0x1
    r = harness.call(app, "f(bool)", 255)
    assert r.abi_return == 1
    # g(bool): 0x0 -> 0x0
    r = harness.call(app, "g(bool)", 0)
    assert r.abi_return == 0
    # g(bool): 0x1 -> 0x1
    r = harness.call(app, "g(bool)", 1)
    assert r.abi_return == 1
    # g(bool): 0x2 -> 0x1
    r = harness.call(app, "g(bool)", 2)
    assert r.abi_return == 1
    # g(bool): 0x3 -> 0x1
    r = harness.call(app, "g(bool)", 3)
    assert r.abi_return == 1
    # g(bool): 0xff -> 0x1
    r = harness.call(app, "g(bool)", 255)
    assert r.abi_return == 1

def test_bool_conversion_v2(harness):
    """cleanup/contracts/bool_conversion_v2.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/bool_conversion_v2.sol")
    # f(bool): 0x0 -> 0x0
    r = harness.call(app, "f(bool)", 0)
    assert r.abi_return == 0
    # f(bool): 0x1 -> 0x1
    r = harness.call(app, "f(bool)", 1)
    assert r.abi_return == 1
    # f(bool): 0x2 -> FAILURE
    r = harness.call(app, "f(bool)", 2, expect_revert=True)
    assert r.reverted
    # f(bool): 0x3 -> FAILURE
    r = harness.call(app, "f(bool)", 3, expect_revert=True)
    assert r.reverted
    # f(bool): 0xff -> FAILURE
    r = harness.call(app, "f(bool)", 255, expect_revert=True)
    assert r.reverted
    # g(bool): 0x0 -> 0x0
    r = harness.call(app, "g(bool)", 0)
    assert r.abi_return == 0
    # g(bool): 0x1 -> 0x1
    r = harness.call(app, "g(bool)", 1)
    assert r.abi_return == 1
    # g(bool): 0x2 -> FAILURE
    r = harness.call(app, "g(bool)", 2, expect_revert=True)
    assert r.reverted
    # g(bool): 0x3 -> FAILURE
    r = harness.call(app, "g(bool)", 3, expect_revert=True)
    assert r.reverted
    # g(bool): 0xff -> FAILURE
    r = harness.call(app, "g(bool)", 255, expect_revert=True)
    assert r.reverted

def test_byte_array_to_storage_cleanup(harness):
    """cleanup/contracts/byte_array_to_storage_cleanup.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/byte_array_to_storage_cleanup.sol")
    # h() -> 0x20, 0x40, 0x00, 0
    r = harness.call(app, "h()")
    assert tuple(r.abi_return) == (32, 64, 0, 0)
    # g() -> 0x20, 0x40, 0, 0x00
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (32, 64, 0, 0)
    # f(bytes): 0x20, 33, 0, -1 -> 0x20, 0x22, 0, 0xff00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes)", 32, 33, 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert tuple(r.abi_return) == (32, 34, 0, 115339776388732929035197660848497720713218148788040405586178452820382218977280)

def test_cleanup_address_types_shortening(harness):
    """cleanup/contracts/cleanup_address_types_shortening.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_address_types_shortening.sol")
    # f() -> 0x1122334455667788990011223344556677889900
    r = harness.call(app, "f()")
    assert r.abi_return == 97815534420055201845582779189627195583443278080
    # g() -> 0x1122334455667788990011223344556677889900
    r = harness.call(app, "g()")
    assert r.abi_return == 97815534420055201845582779189627195583443278080

def test_cleanup_address_types_v1(harness):
    """cleanup/contracts/cleanup_address_types_v1.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_address_types_v1.sol")
    # f(address): 0xffff1234567890123456789012345678901234567890 -> 0x0 # We input longer data on purpose.#
    r = harness.call(app, "f(address)", 0xffff1234567890123456789012345678901234567890)
    # TODO: verify expected: 0x0 # We input longer data on purpose.#
    assert not r.reverted
    # g(address): 0xffff1234567890123456789012345678901234567890 -> 0x0
    r = harness.call(app, "g(address)", 0xffff1234567890123456789012345678901234567890)
    assert r.abi_return == 0

def test_cleanup_address_types_v2(harness):
    """cleanup/contracts/cleanup_address_types_v2.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_address_types_v2.sol")
    # f(address): 0xffff1234567890123456789012345678901234567890 -> FAILURE # We input longer data on purpose.#
    r = harness.call(app, "f(address)", 0xffff1234567890123456789012345678901234567890, expect_revert=True)
    assert r.reverted
    # g(address): 0xffff1234567890123456789012345678901234567890 -> FAILURE
    r = harness.call(app, "g(address)", 0xffff1234567890123456789012345678901234567890, expect_revert=True)
    assert r.reverted

def test_cleanup_bytes_types_shortening_OldCodeGen(harness):
    """cleanup/contracts/cleanup_bytes_types_shortening_OldCodeGen.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_bytes_types_shortening_OldCodeGen.sol")
    # f() -> 0xffffffff00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 115792089210356248756420345214020892766250353992003419616917011526809519390720

def test_cleanup_bytes_types_shortening_newCodeGen(harness):
    """cleanup/contracts/cleanup_bytes_types_shortening_newCodeGen.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_bytes_types_shortening_newCodeGen.sol", via_yul_behavior=True)
    # f() -> 0xffff000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 115790322390251417039241401711187164934754157181743688420499462401711837020160

def test_cleanup_bytes_types_v1(harness):
    """cleanup/contracts/cleanup_bytes_types_v1.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_bytes_types_v1.sol")
    # f(bytes2,uint16): "abc", 0x40102 -> 0x0 # We input longer data on purpose. #
    r = harness.call(app, "f(bytes2,uint16)", bytes.fromhex('616263'), 262402)
    # TODO: verify expected: 0x0 # We input longer data on purpose. #
    assert not r.reverted

def test_cleanup_bytes_types_v2(harness):
    """cleanup/contracts/cleanup_bytes_types_v2.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_bytes_types_v2.sol")
    # f(bytes2,uint16): "abc", 0x40102 -> FAILURE # We input longer data on purpose. #
    r = harness.call(app, "f(bytes2,uint16)", bytes.fromhex('616263'), 262402, expect_revert=True)
    assert r.reverted

def test_cleanup_in_compound_assign(harness):
    """cleanup/contracts/cleanup_in_compound_assign.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_in_compound_assign.sol")
    # test() -> 0xff, 0xff
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (255, 255)

def test_dirty_calldata_bytes(harness):
    """cleanup/contracts/dirty_calldata_bytes.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/dirty_calldata_bytes.sol")
    # f(bytes): 0x20, 0x04, "dead" -> true
    r = harness.call(app, "f(bytes)", 32, 4, bytes.fromhex('64656164'))
    assert r.abi_return is True

def test_dirty_calldata_dynamic_array(harness):
    """cleanup/contracts/dirty_calldata_dynamic_array.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/dirty_calldata_dynamic_array.sol")
    # f(int16[]): 0x20, 0x02, 0x7fff, 0x7fff -> true
    r = harness.call(app, "f(int16[])", 32, 2, 32767, 32767)
    assert r.abi_return is True

def test_exp_cleanup(harness):
    """cleanup/contracts/exp_cleanup.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert r.abi_return == 1

def test_exp_cleanup_direct(harness):
    """cleanup/contracts/exp_cleanup_direct.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_direct.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert r.abi_return == 1

def test_exp_cleanup_nonzero_base(harness):
    """cleanup/contracts/exp_cleanup_nonzero_base.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_nonzero_base.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert r.abi_return == 1

def test_exp_cleanup_smaller_base(harness):
    """cleanup/contracts/exp_cleanup_smaller_base.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_smaller_base.sol")
    # f() -> 0x00
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_indexed_log_topic_during_explicit_downcast(harness):
    """cleanup/contracts/indexed_log_topic_during_explicit_downcast.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/indexed_log_topic_during_explicit_downcast.sol", via_yul_behavior=True)
    # f() -> 0x31
    r = harness.call(app, "f()")
    assert r.abi_return == 49
    # g() -> 0x3100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert r.abi_return == 22163329580580053030292883849319169862539958002407764210677428189014622470144
    # h() -> 0xff00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h()")
    assert r.abi_return == 115339776388732929035197660848497720713218148788040405586178452820382218977280

def test_indexed_log_topic_during_explicit_downcast_during_emissions(harness):
    """cleanup/contracts/indexed_log_topic_during_explicit_downcast_during_emissions.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/indexed_log_topic_during_explicit_downcast_during_emissions.sol")
    # j() ->
    r = harness.call(app, "j()")
    # (void return — call succeeding is the assertion)
