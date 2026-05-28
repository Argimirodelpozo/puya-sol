"""Tests for the saltedCreate category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

def test_prediction_example(harness):
    """saltedCreate/contracts/prediction_example.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy('saltedCreate/contracts/prediction_example.sol')
    r = harness.call(app, 'createDSalted(bytes32,uint256)', 42, 64)

def test_salted_create(harness):
    """saltedCreate/contracts/salted_create.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy('saltedCreate/contracts/salted_create.sol')
    r = harness.call(app, 'different_salt()')
    assert r.abi_return is True
    r = harness.call(app, 'same_salt()')
    assert r.abi_return is True

def test_salted_create_with_value(harness):
    """saltedCreate/contracts/salted_create_with_value.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy('saltedCreate/contracts/salted_create_with_value.sol')
