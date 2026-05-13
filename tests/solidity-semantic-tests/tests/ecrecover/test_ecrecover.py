"""Auto-generated tests for the ecrecover category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_ecrecover(harness):
    """ecrecover/contracts/ecrecover.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/ecrecover.sol")
    # a(bytes32,uint8,bytes32,bytes32): 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549 -> 0xa94f5374fce5edbc8e2a8697c15331677e6ebf0b
    r = harness.call(app, "a(bytes32,uint8,bytes32,bytes32)", 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549)
    assert r.abi_return == 966588469268559010541288244128342317224451555083

def test_ecrecover_abiV2(harness):
    """ecrecover/contracts/ecrecover_abiV2.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/ecrecover_abiV2.sol")
    # a(bytes32,uint8,bytes32,bytes32): 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549 -> 0xa94f5374fce5edbc8e2a8697c15331677e6ebf0b
    r = harness.call(app, "a(bytes32,uint8,bytes32,bytes32)", 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549)
    assert r.abi_return == 966588469268559010541288244128342317224451555083

def test_failing_ecrecover_invalid_input(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_failing_ecrecover_invalid_input_asm(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input_asm.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input_asm.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_failing_ecrecover_invalid_input_proper(harness):
    """ecrecover/contracts/failing_ecrecover_invalid_input_proper.sol"""
    app = harness.compile_and_deploy("ecrecover/contracts/failing_ecrecover_invalid_input_proper.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
