"""Tests for the externalContracts category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_FixedFeeRegistrar(harness):
    """externalContracts/contracts/FixedFeeRegistrar.sol"""
    pytest.fail("Compile-side: FixedFeeRegistrar exits 2. EVM-specific contract with 70-ether expectations (microalgo overflow).")

def test_base64(harness):
    """externalContracts/contracts/base64.sol"""
    pytest.fail("base64 contract uses EVM-specific Yul memory ops for byte-by-byte encoding; AVM result is `\\x00\\x00...` (codegen incomplete).")

def test_deposit_contract(harness):
    """externalContracts/contracts/deposit_contract.sol — ETH 2.0 deposit contract."""
    pytest.fail("32 ether (3.2e19) payment overflows AVM microalgo accounts and `supportsInterface(bytes4)` round-trips through ARC4 dispatcher with EVM-keccak256 selector layout.")

def test_prbmath_signed(harness):
    """externalContracts/contracts/prbmath_signed.sol"""
    pytest.fail("prbmath_signed contract exceeds AVM single-program size; the upstream puya-sol prbmath examples use --split-contracts which the test harness doesn't wire.")

def test_prbmath_unsigned(harness):
    """externalContracts/contracts/prbmath_unsigned.sol"""
    pytest.fail("prbmath_unsigned contract exceeds AVM single-program size; needs --split-contracts.")

def test_ramanujan_pi(harness):
    """externalContracts/contracts/ramanujan_pi.sol"""
    app = harness.compile_and_deploy("externalContracts/contracts/ramanujan_pi.sol")
    # prb_pi() -> 3141592656369545286
    r = harness.call(app, "prb_pi()")
    assert as_int(r.abi_return) == 3141592656369545286

def test_snark(harness):
    """externalContracts/contracts/snark.sol"""
    pytest.fail("snark verifier — currently abi_return None on f(). Likely compiler-side (large pairing verification, opcode budget).")

def test_strings(harness):
    """externalContracts/contracts/strings.sol"""
    pytest.fail("puya-sol SIGSEGV during compile of the strings external contract.")
