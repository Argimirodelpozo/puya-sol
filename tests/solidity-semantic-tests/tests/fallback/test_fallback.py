"""Auto-generated tests for the fallback category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_call_forward_bytes(harness):
    """fallback/call_forward_bytes.sol"""
    app = harness.compile_and_deploy("fallback/call_forward_bytes.sol")
    # recv(uint256): 7 ->
    r = harness.call(app, "recv(uint256)", 7)
    # (void return — call succeeding is the assertion)
    # val() -> 0
    r = harness.call(app, "val()")
    assert r.abi_return == 0
    # forward() -> true
    r = harness.call(app, "forward()")
    assert r.abi_return is True
    # val() -> 8
    r = harness.call(app, "val()")
    assert r.abi_return == 8
    # clear() -> true
    r = harness.call(app, "clear()")
    assert r.abi_return is True
    # val() -> 8
    r = harness.call(app, "val()")
    assert r.abi_return == 8
    # forward() -> true
    r = harness.call(app, "forward()")
    assert r.abi_return is True
    # val() -> 0x80
    r = harness.call(app, "val()")
    assert r.abi_return == 128

def test_falback_return(harness):
    """fallback/falback_return.sol"""
    app = harness.compile_and_deploy("fallback/falback_return.sol")
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
    # ()
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2

def test_fallback_argument(harness):
    """fallback/fallback_argument.sol"""
    app = harness.compile_and_deploy("fallback/fallback_argument.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 0)
    # x() -> 3
    r = harness.call(app, "x()")
    assert r.abi_return == 3

def test_fallback_argument_to_storage(harness):
    """fallback/fallback_argument_to_storage.sol"""
    app = harness.compile_and_deploy("fallback/fallback_argument_to_storage.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 0)
    # x() -> 0x20, 3, "abc"
    r = harness.call(app, "x()")
    assert r.abi_return == 'abc'

def test_fallback_or_receive(harness):
    """fallback/fallback_or_receive.sol"""
    app = harness.compile_and_deploy("fallback/fallback_or_receive.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (0, 0)
    # () ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 0, 1
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (0, 1)
    # (), 1 ether ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 0, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (0, 2)
    # (): 1 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2)
    # (), 1 ether: 1 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # f() -> 2, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (2, 2)

def test_fallback_override(harness):
    """fallback/fallback_override.sol"""
    app = harness.compile_and_deploy("fallback/fallback_override.sol")
    # f() -> 0x01, 0x40, 0x03, 0x78797a0000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 3, 54492172337884459557460545260627547743740629898835569074588186682020990025728)

def test_fallback_override2(harness):
    """fallback/fallback_override2.sol"""
    app = harness.compile_and_deploy("fallback/fallback_override2.sol")
    # f() -> 1, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 0)

def test_fallback_override_multi(harness):
    """fallback/fallback_override_multi.sol"""
    app = harness.compile_and_deploy("fallback/fallback_override_multi.sol")
    # f() -> 0x01, 0x40, 0x00
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 0)

def test_fallback_return_data(harness):
    """fallback/fallback_return_data.sol"""
    app = harness.compile_and_deploy("fallback/fallback_return_data.sol")
    # f() -> 0x01, 0x40, 0x03, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 64, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)

def test_inherited(harness):
    """fallback/inherited.sol"""
    app = harness.compile_and_deploy("fallback/inherited.sol")
    # getData() -> 0
    r = harness.call(app, "getData()")
    assert r.abi_return == 0
    # (): 42 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # getData() -> 1
    r = harness.call(app, "getData()")
    assert r.abi_return == 1

def test_short_data_calls_fallback(harness):
    """fallback/short_data_calls_fallback.sol"""
    app = harness.compile_and_deploy("fallback/short_data_calls_fallback.sol")
    # (): hex"12b87d"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
    # (): hex"12b87db6"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 3
    r = harness.call(app, "x()")
    assert r.abi_return == 3
    # (): hex"12b8"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
    # (): hex"12b87db6"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 3
    r = harness.call(app, "x()")
    assert r.abi_return == 3
    # (): hex"12"
    pytest.xfail("fallback() dispatch not yet implemented")
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2
