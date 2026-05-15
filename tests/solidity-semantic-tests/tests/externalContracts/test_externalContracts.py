"""Tests for the externalContracts category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_FixedFeeRegistrar(harness):
    """externalContracts/contracts/FixedFeeRegistrar.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/FixedFeeRegistrar.sol')

def test_base64(harness):
    """externalContracts/contracts/base64.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/base64.sol')
    r = harness.call(app, 'encode_inline_asm(bytes)', 0x20, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0,)
    r = harness.call(app, 'encode_no_asm(bytes)', 0x20, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0,)
    r = harness.call(app, 'encode_inline_asm_large()')
    r = harness.call(app, 'encode_no_asm_large()')

def test_deposit_contract(harness):
    """externalContracts/contracts/deposit_contract.sol — ETH 2.0 deposit contract."""
    app = harness.compile_and_deploy('externalContracts/contracts/deposit_contract.sol — ETH 2.0 deposit contract.')

def test_prbmath_signed(harness):
    """externalContracts/contracts/prbmath_signed.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/prbmath_signed.sol')
    r = harness.call(app, 'div(int256,int256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 35412542528203691288251815328
    r = harness.call(app, 'exp(int256)', 3141592653589793238)
    assert as_int(r.abi_return) == 23140692632779268978
    r = harness.call(app, 'exp2(int256)', 3141592653589793238)
    assert as_int(r.abi_return) == 8824977827076287620
    r = harness.call(app, 'gm(int256,int256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 16694419339601
    r = harness.call(app, 'log10(int256)', 3141592653589793238)
    assert as_int(r.abi_return) == 4971498726941338506
    r = harness.call(app, 'log2(int256)', 3141592653589793238)
    assert as_int(r.abi_return) == 1651496129472318782
    r = harness.call(app, 'mul(int256,int256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 278703637
    r = harness.call(app, 'pow(int256,uint256)', 3141592653589793238, 5)
    assert as_int(r.abi_return) == 306019684785281453040
    r = harness.call(app, 'sqrt(int256)', 3141592653589793238)
    assert as_int(r.abi_return) == 1772453850905516027
    r = harness.call(app, 'benchmark(int256)', 3141592653589793238)
    assert tuple(as_int(x) for x in r.abi_return) == (998882724338592125, 1000000000000000000, 1000000000000000000,)

def test_prbmath_unsigned(harness):
    """externalContracts/contracts/prbmath_unsigned.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/prbmath_unsigned.sol')
    r = harness.call(app, 'div(uint256,uint256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 35412542528203691288251815328
    r = harness.call(app, 'exp(uint256)', 3141592653589793238)
    assert as_int(r.abi_return) == 23140692632779268978
    r = harness.call(app, 'exp2(uint256)', 3141592653589793238)
    assert as_int(r.abi_return) == 8824977827076287620
    r = harness.call(app, 'gm(uint256,uint256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 16694419339601
    r = harness.call(app, 'log10(uint256)', 3141592653589793238)
    assert as_int(r.abi_return) == 0x44fe4fc084a52b8a
    r = harness.call(app, 'log2(uint256)', 3141592653589793238)
    assert as_int(r.abi_return) == 1651496129472318782
    r = harness.call(app, 'mul(uint256,uint256)', 3141592653589793238, 88714123)
    assert as_int(r.abi_return) == 278703637
    r = harness.call(app, 'pow(uint256,uint256)', 3141592653589793238, 5)
    assert as_int(r.abi_return) == 306019684785281453040
    r = harness.call(app, 'sqrt(uint256)', 3141592653589793238)
    assert as_int(r.abi_return) == 1772453850905516027
    r = harness.call(app, 'benchmark(uint256)', 3141592653589793238)
    assert tuple(as_int(x) for x in r.abi_return) == (998882724338592125, 1000000000000000000, 1000000000000000000,)

def test_ramanujan_pi(harness):
    """externalContracts/contracts/ramanujan_pi.sol"""
    app = harness.compile_and_deploy("externalContracts/contracts/ramanujan_pi.sol")
    # prb_pi() -> 3141592656369545286
    r = harness.call(app, "prb_pi()")
    assert as_int(r.abi_return) == 3141592656369545286

def test_snark(harness):
    """externalContracts/contracts/snark.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/snark.sol')

def test_strings(harness):
    """externalContracts/contracts/strings.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/strings.sol')
