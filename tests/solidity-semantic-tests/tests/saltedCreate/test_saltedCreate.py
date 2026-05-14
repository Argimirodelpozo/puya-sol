"""Tests for the saltedCreate category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

def test_prediction_example(harness):
    """saltedCreate/contracts/prediction_example.sol"""

    pytest.fail("EVM CREATE2 address prediction (keccak256(0xff, deployer, salt, init_code)). AVM app IDs are sequential; no salt-based address prediction.")
def test_salted_create(harness):
    """saltedCreate/contracts/salted_create.sol"""

    pytest.fail("EVM CREATE2 address prediction (keccak256(0xff, deployer, salt, init_code)). AVM app IDs are sequential; no salt-based address prediction.")
def test_salted_create_with_value(harness):
    """saltedCreate/contracts/salted_create_with_value.sol"""
    pytest.fail("EVM CREATE2 address prediction (keccak256(0xff, deployer, salt, init_code)). AVM app IDs are sequential; no salt-based address prediction.")
