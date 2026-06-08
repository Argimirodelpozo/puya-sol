"""Tests for the externalContracts category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_FixedFeeRegistrar(harness):  # currently fails
    """externalContracts/contracts/FixedFeeRegistrar.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/FixedFeeRegistrar.sol')

@pytest.mark.xfail(reason="aggregate (struct/array/bytes/string) used as a value in inline assembly is its Yul memory pointer; puya-sol models memory aggregates as native ARC4 values with no linear-memory offset, so this is a hard compile error per EVM_DIVERGENCE.md (#13) (was a silent coerce-to-biguint(0))", strict=False)
def test_base64(harness):
    """externalContracts/contracts/base64.sol

    encode_no_asm (plain-Solidity Base64) is exercised across the full
    RFC4648 vector set. encode_inline_asm walks raw EVM memory pointers
    (mload/mstore8) in Yul; only its empty-input case is checked here —
    non-empty inputs need EVM-memory-model fidelity beyond puya-sol's
    blob-backed memory model.
    """
    app = harness.compile_and_deploy('externalContracts/contracts/base64.sol')
    # encode_no_asm — RFC4648 §10 vectors.
    for src, want in [
        (b"", ""), (b"f", "Zg=="), (b"fo", "Zm8="), (b"foo", "Zm9v"),
        (b"foob", "Zm9vYg=="), (b"fooba", "Zm9vYmE="), (b"foobar", "Zm9vYmFy"),
    ]:
        r = harness.call(app, 'encode_no_asm(bytes)', src)
        assert r.abi_return == want
    # encode_inline_asm — empty input only (see docstring).
    r = harness.call(app, 'encode_inline_asm(bytes)', b"")
    assert r.abi_return == ""

def test_deposit_contract(harness):  # currently fails
    """externalContracts/contracts/deposit_contract.sol — ETH 2.0 deposit contract."""
    app = harness.compile_and_deploy('externalContracts/contracts/deposit_contract.sol — ETH 2.0 deposit contract.')

def test_prbmath_signed(harness):  # currently fails
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

def test_prbmath_unsigned(harness):  # currently fails
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

@pytest.mark.xfail(reason="aggregate (struct/array/bytes/string) used as a value in inline assembly is its Yul memory pointer; puya-sol models memory aggregates as native ARC4 values with no linear-memory offset, so this is a hard compile error per EVM_DIVERGENCE.md (#13) (was a silent coerce-to-biguint(0))", strict=False)
def test_strings(harness):
    """externalContracts/contracts/strings.sol"""
    app = harness.compile_and_deploy('externalContracts/contracts/strings.sol')
