"""Tests for the ecrecover category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


_ECR_HASH = (0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c).to_bytes(32, "big")
_ECR_R = (0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f).to_bytes(32, "big")
_ECR_S = (0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549).to_bytes(32, "big")
_ECR_EXPECTED = 0xa94f5374fce5edbc8e2a8697c15331677e6ebf0b


def test_ecrecover(harness):
    """ecrecover/contracts/ecrecover.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/ecrecover.sol")
    r = harness.call(app, "a(bytes32,uint8,bytes32,bytes32)", _ECR_HASH, 28, _ECR_R, _ECR_S)
    assert as_int(r.abi_return) == _ECR_EXPECTED


def test_ecrecover_abiV2(harness):
    """ecrecover/contracts/ecrecover_abiV2.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/ecrecover_abiV2.sol")
    r = harness.call(app, "a(bytes32,uint8,bytes32,bytes32)", _ECR_HASH, 28, _ECR_R, _ECR_S)
    assert as_int(r.abi_return) == _ECR_EXPECTED

def test_failing_ecrecover_invalid_input(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_failing_ecrecover_invalid_input_asm(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input_asm.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input_asm.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_failing_ecrecover_invalid_input_proper(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input_proper.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input_proper.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
