"""Tests for the state category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)
from framework.compile import CompileError


def test_blobhash(harness):
    """EIP-4844 blob hashes have no AVM execution-context equivalent."""
    with pytest.raises(CompileError):
        harness.compile_and_deploy("state/contracts/blobhash.sol")

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
    app = harness.compile_and_deploy(
        "state/contracts/block_coinbase.sol",
        extra_args=["--evm-coinbase", "0x1111111111111111111111111111111111111111"],
    )
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
    """state/contracts/block_difficulty.sol — difficulty == prevrandao
    post-Paris (same EVM opcode): the Algorand block seed, never zero."""
    app = harness.compile_and_deploy("state/contracts/block_difficulty.sol", evm_version='london')
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) != 0

def test_block_difficulty_post_paris(harness):
    """state/contracts/block_difficulty_post_paris.sol — same block-seed
    lowering as block.prevrandao."""
    app = harness.compile_and_deploy("state/contracts/block_difficulty_post_paris.sol")
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) != 0

def test_block_gaslimit(harness):
    """state/contracts/block_gaslimit.sol"""
    app = harness.compile_and_deploy(
        "state/contracts/block_gaslimit.sol",
        extra_args=["--evm-block-gas-limit", "20000000"],
    )
    # Explicitly replay the upstream EVM environment value.
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 20000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 20000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 20000000

def test_block_gaslimit_unconfigured(harness):
    """Without --evm-block-gas-limit: the group's TOTAL pooled app-call
    opcode budget (GroupSize x 700) — constant within an execution, unlike
    the shrinking OpcodeBudget remainder. Single-txn group -> 700."""
    app = harness.compile_and_deploy("state/contracts/block_gaslimit.sol")
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 700
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 700

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

@pytest.mark.xfail(reason="blockhash(n) is now a hard compile error per EVM_DIVERGENCE.md — AVM has no block-hash opcode; `block BlkSeed` is a per-round VRF seed that ignores the round argument and panics out-of-window, with no faithful equivalent", strict=False)
def test_blockhash_basic(harness):
    """state/contracts/blockhash_basic.sol"""
    app = harness.compile_and_deploy('state/contracts/blockhash_basic.sol')
    r = harness.call(app, 'genesisHash()')
    assert as_int(r.abi_return) == 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, 'currentHash()')
    assert as_int(r.abi_return) == 0
    r = harness.call(app, 'f(uint256)', 0)
    assert as_int(r.abi_return) == 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, 'f(uint256)', 1)
    assert as_int(r.abi_return) == 0x3737373737373737373737373737373737373737373737373737373737373738
    r = harness.call(app, 'f(uint256)', 255)
    assert as_int(r.abi_return) == 0x00
    r = harness.call(app, 'f(uint256)', 256)
    assert as_int(r.abi_return) == 0x00
    r = harness.call(app, 'f(uint256)', 257)
    assert as_int(r.abi_return) == 0x00

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

def test_msg_data(harness):
    """state/contracts/msg_data.sol

    ARCH NOTE: EVM msg.data is a contiguous calldata blob
    (selector + abi-encoded args). AVM ApplicationArgs are slot-based;
    puya-sol's msg.data implementation reconstructs an EVM-style blob by
    concatenating ApplicationArgs[0] (selector) with ApplicationArgs[1..].
    Selectors differ (ARC4 sha512_256 vs EVM keccak256) and individual
    arg encodings follow ARC4 widths (uint256→32B, bool→1B), so the exact
    bytes won't match the EVM expected values — but the layout is
    reconstructible.
    """
    app = harness.compile_and_deploy("state/contracts/msg_data.sol")
    # f() takes no args → msg.data is exactly the 4-byte selector.
    r = harness.call(app, "f()")
    assert len(bytes(r.abi_return)) == 4
    # g(uint256, bool): selector (4) + uint256 (32) + bool (1) = 37 bytes.
    r = harness.call(app, "g(uint256,bool)", 1234, True)
    assert len(bytes(r.abi_return)) == 4 + 32 + 1

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

@pytest.mark.xfail(reason="tx.origin is now a hard compile error per EVM_DIVERGENCE.md — on AVM it would silently alias msg.sender, making tx.origin (==|!=) msg.sender guards vacuous", strict=False)
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

@pytest.mark.xfail(reason="blobhash(n) is a hard compile error per EvmFeaturePolicy — AVM has no blob transactions; every function is translated (no pre-translation DCE), so even an uncalled blobhash is rejected at compile time", strict=False)
def test_uncalled_blobhash(harness):
    """state/contracts/uncalled_blobhash.sol — EVM blobhash opcode has no
    AVM analog; verify the call doesn't revert."""
    app = harness.compile_and_deploy("state/contracts/uncalled_blobhash.sol")
    assert not harness.call(app, "f()").reverted

@pytest.mark.xfail(reason="blockhash(n) is now a hard compile error per EVM_DIVERGENCE.md — every function is translated (no pre-translation DCE), so even an uncalled blockhash is rejected at compile time", strict=False)
def test_uncalled_blockhash(harness):
    """state/contracts/uncalled_blockhash.sol — EVM blockhash; verify success."""
    app = harness.compile_and_deploy("state/contracts/uncalled_blockhash.sol")
    assert not harness.call(app, "f()").reverted
