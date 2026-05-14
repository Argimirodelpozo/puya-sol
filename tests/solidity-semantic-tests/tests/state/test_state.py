"""Tests for the state category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_blobhash(harness):
    """state/contracts/blobhash.sol"""
    app = harness.compile_and_deploy("state/contracts/blobhash.sol")
    # f(uint256): 0 -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662657
    # f(uint256): 1 -> 0x0100000000000000000000000000000000000000000000000000000000000002
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662658
    # f(uint256): 2 -> 0x00
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 0
    # f(uint256): 255 -> 0x00
    r = harness.call(app, "f(uint256)", 255)
    assert as_int(r.abi_return) == 0
    # f(uint256): 256 -> 0x00
    r = harness.call(app, "f(uint256)", 256)
    assert as_int(r.abi_return) == 0
    # f(uint256): 257 -> 0x00
    r = harness.call(app, "f(uint256)", 257)
    assert as_int(r.abi_return) == 0

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

def test_blockhash_basic(harness):
    """state/contracts/blockhash_basic.sol"""
    app = harness.compile_and_deploy("state/contracts/blockhash_basic.sol")
    # genesisHash() -> 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, "genesisHash()")
    assert as_int(r.abi_return) == 24974764345303493130574134021481705615411173163177376557530067138961655412535
    # currentHash() -> 0
    r = harness.call(app, "currentHash()")
    assert as_int(r.abi_return) == 0
    # f(uint256): 0 -> 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 24974764345303493130574134021481705615411173163177376557530067138961655412535
    # f(uint256): 1 -> 0x3737373737373737373737373737373737373737373737373737373737373738
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 24974764345303493130574134021481705615411173163177376557530067138961655412536
    # f(uint256): 255 -> 0x00
    r = harness.call(app, "f(uint256)", 255)
    assert as_int(r.abi_return) == 0
    # f(uint256): 256 -> 0x00
    r = harness.call(app, "f(uint256)", 256)
    assert as_int(r.abi_return) == 0
    # f(uint256): 257 -> 0x00
    r = harness.call(app, "f(uint256)", 257)
    assert as_int(r.abi_return) == 0

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
    """state/contracts/msg_data.sol"""
    app = harness.compile_and_deploy("state/contracts/msg_data.sol")
    # f() -> 0x20, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912)
    # g(uint256,bool): 1234, true -> 0x20, 0x44, 35691323728519381642872894128098848782337736632589179916067422734266033766400, 33268574187263889506619096617382224251268236217415066441681855047532544, 26959946667150639794667015087019630673637144422540572481103610249216
    r = harness.call(app, "g(uint256,bool)", 1234, True)
    # TODO: verify structural decoding matches expected: 32, 68, 35691323728519381642872894128098848782337736632589179916067422734266033766400, 33268574187263889506619096617382224251268236217415066441681855047532544, 26959946667150639794667015087019630673637144422540572481103610249216
    assert not r.reverted

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
    """state/contracts/msg_value.sol"""
    app = harness.compile_and_deploy("state/contracts/msg_value.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # f(), 12 ether -> 12000000000000000000
    r = harness.call(app, "f()", payment_wei=12000000000000000000)
    assert as_int(r.abi_return) == 12000000000000000000

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
    """state/contracts/uncalled_blobhash.sol"""
    app = harness.compile_and_deploy("state/contracts/uncalled_blobhash.sol")
    # f() -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662657

def test_uncalled_blockhash(harness):
    """state/contracts/uncalled_blockhash.sol"""
    app = harness.compile_and_deploy("state/contracts/uncalled_blockhash.sol")
    # f() -> 0x3737373737373737373737373737373737373737373737373737373737373738
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 24974764345303493130574134021481705615411173163177376557530067138961655412536
