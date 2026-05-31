"""Tests for the shanghai category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


@pytest.mark.xfail(reason="uses address(other).code (arbitrary address) — now a hard compile error per EVM_DIVERGENCE.md; AVM can't dereference an arbitrary address to its application program. address(this).code is supported", strict=False)
def test_evmone_support(harness):
    """shanghai/contracts/evmone_support.sol"""
    pytest.xfail("`address.code` / raw EVM bytecode calls have no AVM equivalent. "
                 "ShortReturn deploys a 4-byte EVM bytecode blob via assembly return(0,4); "
                 "bytecode() uses address.code (EXTCODECOPY) and isPush0Supported() calls "
                 "into that raw EVM blob — neither operation is possible on AVM.")
    app = harness.compile_and_deploy('shanghai/contracts/evmone_support.sol')
    r = harness.call(app, 'bytecode()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 4, 0x60205ff300000000000000000000000000000000000000000000000000000000,)
    r = harness.call(app, 'isPush0Supported()')
    assert r.abi_return is True

def test_push0(harness):
    """shanghai/contracts/push0.sol"""
    app = harness.compile_and_deploy("shanghai/contracts/push0.sol")
    # zero() -> 0
    r = harness.call(app, "zero()")
    assert as_int(r.abi_return) == 0
