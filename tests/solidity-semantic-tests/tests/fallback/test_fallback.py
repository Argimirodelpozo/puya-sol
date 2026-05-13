"""Tests for the fallback category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_call_forward_bytes(harness):
    """fallback/contracts/call_forward_bytes.sol"""
    app = harness.compile_and_deploy("fallback/contracts/call_forward_bytes.sol")
    # recv(uint256): 7 ->
    r = harness.call(app, "recv(uint256)", 7)
    # (void return — call succeeding is the assertion)
    # val() -> 0
    r = harness.call(app, "val()")
    assert as_int(r.abi_return) == 0
    # forward() -> true
    r = harness.call(app, "forward()")
    assert bool(as_int(r.abi_return)) is True
    # val() -> 8
    r = harness.call(app, "val()")
    assert as_int(r.abi_return) == 8
    # clear() -> true
    r = harness.call(app, "clear()")
    assert bool(as_int(r.abi_return)) is True
    # val() -> 8
    r = harness.call(app, "val()")
    assert as_int(r.abi_return) == 8
    # forward() -> true
    r = harness.call(app, "forward()")
    assert bool(as_int(r.abi_return)) is True
    # val() -> 0x80
    r = harness.call(app, "val()")
    assert as_int(r.abi_return) == 128

def test_falback_return(harness):
    """fallback/contracts/falback_return.sol"""
    app = harness.compile_and_deploy("fallback/contracts/falback_return.sol")
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2

def test_fallback_argument(harness):
    """fallback/contracts/fallback_argument.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_argument.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 0)
    # x() -> 3
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 3

def test_fallback_argument_to_storage(harness):
    """fallback/contracts/fallback_argument_to_storage.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_argument_to_storage.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 0)
    # x() -> 0x20, 3, "abc"
    r = harness.call(app, "x()")
    assert r.abi_return == 'abc'

def test_fallback_or_receive(harness):
    """fallback/contracts/fallback_or_receive.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_or_receive.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # () ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 0, 1
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 1)
    # (), 1 ether ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 0, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2)
    # (): 1 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # (), 1 ether: 1 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 2, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 2)

def test_fallback_override(harness):
    """fallback/contracts/fallback_override.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override.sol")
    # f() -> 0x01, 0x40, 0x03, 0x78797a0000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 3, 54492172337884459557460545260627547743740629898835569074588186682020990025728)

def test_fallback_override2(harness):
    """fallback/contracts/fallback_override2.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override2.sol")
    # f() -> 1, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 0)

def test_fallback_override_multi(harness):
    """fallback/contracts/fallback_override_multi.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_override_multi.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 0)

def test_fallback_return_data(harness):
    """fallback/contracts/fallback_return_data.sol"""
    app = harness.compile_and_deploy("fallback/contracts/fallback_return_data.sol")
    # f() -> 0x01, 0x40, 0x03, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 64, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)

def test_inherited(harness):
    """fallback/contracts/inherited.sol"""
    app = harness.compile_and_deploy("fallback/contracts/inherited.sol")
    # getData() -> 0
    r = harness.call(app, "getData()")
    assert as_int(r.abi_return) == 0
    # (): 42 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # getData() -> 1
    r = harness.call(app, "getData()")
    assert as_int(r.abi_return) == 1

def test_short_data_calls_fallback(harness):
    """fallback/contracts/short_data_calls_fallback.sol"""
    app = harness.compile_and_deploy("fallback/contracts/short_data_calls_fallback.sol")
    # (): hex"12b87d"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # (): hex"12b87db6"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 3
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 3
    # (): hex"12b8"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # (): hex"12b87db6"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 3
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 3
    # (): hex"12"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
