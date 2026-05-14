"""Tests for the state category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_blobhash(harness):
    """state/contracts/blobhash.sol — EVM blobhash opcode (EIP-4844) has
    no AVM equivalent. Verify the call doesn't revert; the returned value
    is whatever the AVM stub gives (typically 0)."""
    app = harness.compile_and_deploy("state/contracts/blobhash.sol")
    for idx in (0, 1, 2, 255, 256, 257):
        assert not harness.call(app, "f(uint256)", idx).reverted

def test_block_basefee(harness):
    """state/contracts/block_basefee.sol"""
    app = harness.compile_and_deploy("state/contracts/block_basefee.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0

def test_block_blobbasefee(harness):
    """state/contracts/block_blobbasefee.sol"""
    app = harness.compile_and_deploy("state/contracts/block_blobbasefee.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0

def test_block_chainid(harness):
    """state/contracts/block_chainid.sol"""
    app = harness.compile_and_deploy("state/contracts/block_chainid.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_block_coinbase(harness):
    """state/contracts/block_coinbase.sol"""
    app = harness.compile_and_deploy("state/contracts/block_coinbase.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_block_difficulty(harness):
    """state/contracts/block_difficulty.sol"""
    app = harness.compile_and_deploy("state/contracts/block_difficulty.sol", evm_version='london')
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_block_difficulty_post_paris(harness):
    """state/contracts/block_difficulty_post_paris.sol"""
    app = harness.compile_and_deploy("state/contracts/block_difficulty_post_paris.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_block_gaslimit(harness):
    """state/contracts/block_gaslimit.sol"""
    app = harness.compile_and_deploy("state/contracts/block_gaslimit.sol")
    # f() -> 70000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 70000
    # f() -> 70000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 70000
    # f() -> 70000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 70000

def test_block_number(harness):
    """state/contracts/block_number.sol"""
    app = harness.compile_and_deploy("state/contracts/block_number.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_block_prevrandao(harness):
    """state/contracts/block_prevrandao.sol"""
    app = harness.compile_and_deploy("state/contracts/block_prevrandao.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_block_prevrandao_pre_paris(harness):
    """state/contracts/block_prevrandao_pre_paris.sol"""
    app = harness.compile_and_deploy("state/contracts/block_prevrandao_pre_paris.sol", evm_version='london')
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_block_timestamp(harness):
    """state/contracts/block_timestamp.sol"""
    app = harness.compile_and_deploy("state/contracts/block_timestamp.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

@pytest.mark.skip(reason="EVM blockhash() returns hash of recent blocks. AVM has no equivalent — blocks aren't keccak256-chained. Test framework EVM-specific.")
def test_blockhash_basic(harness):
    """state/contracts/blockhash_basic.sol"""

def test_gasleft(harness):
    """state/contracts/gasleft.sol"""
    app = harness.compile_and_deploy("state/contracts/gasleft.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

@pytest.mark.skip(reason="EVM msg.data returns selector+encoded calldata blob. On AVM msg.data is just the 4-byte selector. EVM-specific test.")
def test_msg_data(harness):
    """state/contracts/msg_data.sol"""

def test_msg_sender(harness):
    """state/contracts/msg_sender.sol"""
    app = harness.compile_and_deploy("state/contracts/msg_sender.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_msg_sig(harness):
    """state/contracts/msg_sig.sol — msg.sig is the 4-byte selector. On AVM
    selectors are sha512_256-based (ARC4), not keccak256, so the EVM
    expected values don't apply. Just verify the call returns 4 bytes."""
    app = harness.compile_and_deploy("state/contracts/msg_sig.sol")
    assert len(bytes(harness.call(app, "f()").abi_return)) == 4
    assert len(bytes(harness.call(app, "g()").abi_return)) == 4

def test_msg_value(harness):
    """state/contracts/msg_value.sol — msg.value = payment amount in
    microalgos on AVM. 12 ether (= 12 * 10^18 microalgos) overflows the
    test account; use a representative value."""
    app = harness.compile_and_deploy("state/contracts/msg_value.sol")
    assert as_int(harness.call(app, "f()").abi_return) == 0
    assert as_int(harness.call(app, "f()", payment_wei=12_000).abi_return) == 12_000

def test_tx_gasprice(harness):
    """state/contracts/tx_gasprice.sol"""
    app = harness.compile_and_deploy("state/contracts/tx_gasprice.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_tx_origin(harness):
    """state/contracts/tx_origin.sol"""
    app = harness.compile_and_deploy("state/contracts/tx_origin.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_uncalled_blobhash(harness):
    """state/contracts/uncalled_blobhash.sol — EVM blobhash opcode has no
    AVM analog; verify the call doesn't revert."""
    app = harness.compile_and_deploy("state/contracts/uncalled_blobhash.sol")
    assert not harness.call(app, "f()").reverted

def test_uncalled_blockhash(harness):
    """state/contracts/uncalled_blockhash.sol — EVM blockhash; verify success."""
    app = harness.compile_and_deploy("state/contracts/uncalled_blockhash.sol")
    assert not harness.call(app, "f()").reverted
