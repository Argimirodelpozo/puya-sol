"""Tests for the saltedCreate category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

def test_prediction_example(harness):  # currently fails
    """saltedCreate/contracts/prediction_example.sol"""
    app = harness.compile_and_deploy('saltedCreate/contracts/prediction_example.sol')
    r = harness.call(app, 'createDSalted(bytes32,uint256)', 42, 64)

def test_salted_create(harness):  # currently fails
    """saltedCreate/contracts/salted_create.sol"""
    app = harness.compile_and_deploy('saltedCreate/contracts/salted_create.sol')
    r = harness.call(app, 'different_salt()')
    assert r.abi_return is True
    r = harness.call(app, 'same_salt()')
    assert r.abi_return is True

def test_salted_create_with_value(harness):
    """saltedCreate/contracts/salted_create_with_value.sol"""
    app = harness.compile_and_deploy('saltedCreate/contracts/salted_create_with_value.sol')
