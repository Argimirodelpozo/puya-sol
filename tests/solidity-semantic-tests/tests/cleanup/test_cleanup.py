"""Tests for the cleanup category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_bool_conversion_v1(harness):
    """cleanup/contracts/bool_conversion_v1.sol — only valid bool values
    (False/True) are observable via ARC4 dispatch on AVM. The EVM "dirty
    bool" cleanup paths (0x2, 0x3, 0xFF coercing to 1) can't be exercised
    because algosdk's bool encoder rejects non-bool args.
    """
    app = harness.compile_and_deploy("cleanup/contracts/bool_conversion_v1.sol")
    for v in (False, True):
        assert bool(harness.call(app, "f(bool)", v).abi_return) is v
        assert bool(harness.call(app, "g(bool)", v).abi_return) is v

def test_bool_conversion_v2(harness):
    """cleanup/contracts/bool_conversion_v2.sol — only valid bool values
    (False/True) are observable via ARC4 dispatch on AVM. The EVM "dirty
    bool" revert paths can't be exercised because algosdk's bool encoder
    rejects non-bool args.
    """
    app = harness.compile_and_deploy("cleanup/contracts/bool_conversion_v2.sol")
    for v in (False, True):
        assert bool(harness.call(app, "f(bool)", v).abi_return) is v
        assert bool(harness.call(app, "g(bool)", v).abi_return) is v

@pytest.mark.skip(reason="EVM-flat calldata format expected by test. AVM ARC4 has different byte-array encoding.")
def test_byte_array_to_storage_cleanup(harness):
    """cleanup/contracts/byte_array_to_storage_cleanup.sol"""

@pytest.mark.skip(reason="EVM stores address as 20 bytes; expected value is the 160-bit form. AVM addresses are 32-byte; full value won't match the 20-byte EVM form.")
def test_cleanup_address_types_shortening(harness):
    """cleanup/contracts/cleanup_address_types_shortening.sol"""

@pytest.mark.skip(reason="EVM-style 'overlong address gets truncated/cleaned' test — AVM has 32-byte addresses natively, so the EVM 20-byte truncation behavior doesn't apply")
def test_cleanup_address_types_v1(harness):
    """cleanup/contracts/cleanup_address_types_v1.sol"""

@pytest.mark.skip(reason="EVM-style 'overlong address reverts' test — AVM has 32-byte addresses natively")
def test_cleanup_address_types_v2(harness):
    """cleanup/contracts/cleanup_address_types_v2.sol"""

@pytest.mark.skip(reason="EVM 32-byte right-padding of bytes4. AVM returns 4 raw bytes; value as integer differs.")
def test_cleanup_bytes_types_shortening_OldCodeGen(harness):
    """cleanup/contracts/cleanup_bytes_types_shortening_OldCodeGen.sol"""

def test_cleanup_bytes_types_shortening_newCodeGen(harness):
    """cleanup/contracts/cleanup_bytes_types_shortening_newCodeGen.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_bytes_types_shortening_newCodeGen.sol", via_yul_behavior=True)
    # f() -> 0xffff000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 115790322390251417039241401711187164934754157181743688420499462401711837020160

@pytest.mark.skip(reason="EVM-flat 'overlong bytes2/uint16 args truncate/revert' test — algosdk rejects oversized args at encode time, so the dispatcher never sees them")
def test_cleanup_bytes_types_v1(harness):
    """cleanup/contracts/cleanup_bytes_types_v1.sol"""

@pytest.mark.skip(reason="EVM-flat 'overlong bytes2/uint16 args revert' test — algosdk rejects oversized args at encode time")
def test_cleanup_bytes_types_v2(harness):
    """cleanup/contracts/cleanup_bytes_types_v2.sol"""

def test_cleanup_in_compound_assign(harness):
    """cleanup/contracts/cleanup_in_compound_assign.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/cleanup_in_compound_assign.sol")
    # test() -> 0xff, 0xff
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (255, 255)

def test_dirty_calldata_bytes(harness):
    """cleanup/contracts/dirty_calldata_bytes.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/dirty_calldata_bytes.sol")
    r = harness.call(app, "f(bytes)", b"dead")
    assert bool(as_int(r.abi_return)) is True

def test_dirty_calldata_dynamic_array(harness):
    """cleanup/contracts/dirty_calldata_dynamic_array.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/dirty_calldata_dynamic_array.sol")
    r = harness.call(app, "f(int16[])", [32767, 32767])
    assert bool(as_int(r.abi_return)) is True

def test_exp_cleanup(harness):
    """cleanup/contracts/exp_cleanup.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_exp_cleanup_direct(harness):
    """cleanup/contracts/exp_cleanup_direct.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_direct.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_exp_cleanup_nonzero_base(harness):
    """cleanup/contracts/exp_cleanup_nonzero_base.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_nonzero_base.sol")
    # f() -> 0x1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_exp_cleanup_smaller_base(harness):
    """cleanup/contracts/exp_cleanup_smaller_base.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/exp_cleanup_smaller_base.sol")
    # f() -> 0x00
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_indexed_log_topic_during_explicit_downcast(harness):
    """cleanup/contracts/indexed_log_topic_during_explicit_downcast.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/indexed_log_topic_during_explicit_downcast.sol", via_yul_behavior=True)
    # f() -> 0x31
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 49
    # g() -> 0x3100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 22163329580580053030292883849319169862539958002407764210677428189014622470144
    # h() -> 0xff00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 115339776388732929035197660848497720713218148788040405586178452820382218977280

def test_indexed_log_topic_during_explicit_downcast_during_emissions(harness):
    """cleanup/contracts/indexed_log_topic_during_explicit_downcast_during_emissions.sol"""
    app = harness.compile_and_deploy("cleanup/contracts/indexed_log_topic_during_explicit_downcast_during_emissions.sol")
    # j() ->
    r = harness.call(app, "j()")
    # (void return — call succeeding is the assertion)
