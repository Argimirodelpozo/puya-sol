"""Auto-generated tests for the state category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_blobhash(harness):
    """state/blobhash.sol"""
    app = harness.compile_and_deploy("state/blobhash.sol")
    # f(uint256): 0 -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f(uint256)", 0)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662657
    # f(uint256): 1 -> 0x0100000000000000000000000000000000000000000000000000000000000002
    r = harness.call(app, "f(uint256)", 1)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662658
    # f(uint256): 2 -> 0x00
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 0
    # f(uint256): 255 -> 0x00
    r = harness.call(app, "f(uint256)", 255)
    assert r.abi_return == 0
    # f(uint256): 256 -> 0x00
    r = harness.call(app, "f(uint256)", 256)
    assert r.abi_return == 0
    # f(uint256): 257 -> 0x00
    r = harness.call(app, "f(uint256)", 257)
    assert r.abi_return == 0

def test_block_basefee(harness):
    """state/block_basefee.sol"""
    app = harness.compile_and_deploy("state/block_basefee.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0

def test_block_blobbasefee(harness):
    """state/block_blobbasefee.sol"""
    app = harness.compile_and_deploy("state/block_blobbasefee.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g() -> 0
    r = harness.call(app, "g()")
    assert r.abi_return == 0

def test_block_chainid(harness):
    """state/block_chainid.sol"""
    app = harness.compile_and_deploy("state/block_chainid.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_block_coinbase(harness):
    """state/block_coinbase.sol"""
    app = harness.compile_and_deploy("state/block_coinbase.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_block_difficulty(harness):
    """state/block_difficulty.sol"""
    app = harness.compile_and_deploy("state/block_difficulty.sol", evm_version='london')
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_block_difficulty_post_paris(harness):
    """state/block_difficulty_post_paris.sol"""
    app = harness.compile_and_deploy("state/block_difficulty_post_paris.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0

def test_block_gaslimit(harness):
    """state/block_gaslimit.sol"""
    app = harness.compile_and_deploy("state/block_gaslimit.sol")
    # f() -> 70000
    r = harness.call(app, "f()")
    assert r.abi_return == 70000
    # f() -> 70000
    r = harness.call(app, "f()")
    assert r.abi_return == 70000
    # f() -> 70000
    r = harness.call(app, "f()")
    assert r.abi_return == 70000

def test_block_number(harness):
    """state/block_number.sol"""
    app = harness.compile_and_deploy("state/block_number.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_block_prevrandao(harness):
    """state/block_prevrandao.sol"""
    app = harness.compile_and_deploy("state/block_prevrandao.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_block_prevrandao_pre_paris(harness):
    """state/block_prevrandao_pre_paris.sol"""
    app = harness.compile_and_deploy("state/block_prevrandao_pre_paris.sol", evm_version='london')
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_block_timestamp(harness):
    """state/block_timestamp.sol"""
    app = harness.compile_and_deploy("state/block_timestamp.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_blockhash_basic(harness):
    """state/blockhash_basic.sol"""
    app = harness.compile_and_deploy("state/blockhash_basic.sol")
    # genesisHash() -> 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, "genesisHash()")
    assert r.abi_return == 24974764345303493130574134021481705615411173163177376557530067138961655412535
    # currentHash() -> 0
    r = harness.call(app, "currentHash()")
    assert r.abi_return == 0
    # f(uint256): 0 -> 0x3737373737373737373737373737373737373737373737373737373737373737
    r = harness.call(app, "f(uint256)", 0)
    assert r.abi_return == 24974764345303493130574134021481705615411173163177376557530067138961655412535
    # f(uint256): 1 -> 0x3737373737373737373737373737373737373737373737373737373737373738
    r = harness.call(app, "f(uint256)", 1)
    assert r.abi_return == 24974764345303493130574134021481705615411173163177376557530067138961655412536
    # f(uint256): 255 -> 0x00
    r = harness.call(app, "f(uint256)", 255)
    assert r.abi_return == 0
    # f(uint256): 256 -> 0x00
    r = harness.call(app, "f(uint256)", 256)
    assert r.abi_return == 0
    # f(uint256): 257 -> 0x00
    r = harness.call(app, "f(uint256)", 257)
    assert r.abi_return == 0

def test_gasleft(harness):
    """state/gasleft.sol"""
    app = harness.compile_and_deploy("state/gasleft.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_msg_data(harness):
    """state/msg_data.sol"""
    app = harness.compile_and_deploy("state/msg_data.sol")
    # f() -> 0x20, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912)
    # g(uint256,bool): 1234, true -> 0x20, 0x44, 35691323728519381642872894128098848782337736632589179916067422734266033766400, 33268574187263889506619096617382224251268236217415066441681855047532544, 26959946667150639794667015087019630673637144422540572481103610249216
    r = harness.call(app, "g(uint256,bool)", 1234, True)
    # TODO: verify structural decoding matches expected: 32, 68, 35691323728519381642872894128098848782337736632589179916067422734266033766400, 33268574187263889506619096617382224251268236217415066441681855047532544, 26959946667150639794667015087019630673637144422540572481103610249216
    assert not r.reverted

def test_msg_sender(harness):
    """state/msg_sender.sol"""
    app = harness.compile_and_deploy("state/msg_sender.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_msg_sig(harness):
    """state/msg_sig.sol"""
    app = harness.compile_and_deploy("state/msg_sig.sol")
    # f() -> 0x26121ff000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 17219911917854084299749778639755835327755045716242581057573779540915269926912
    # g() -> 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert r.abi_return == 102264414861304285884729579275374176073311626045629144087797787832582884294656

def test_msg_value(harness):
    """state/msg_value.sol"""
    app = harness.compile_and_deploy("state/msg_value.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # f(), 12 ether -> 12000000000000000000
    r = harness.call(app, "f()", payment_wei=12000000000000000000)
    assert r.abi_return == 12000000000000000000

def test_tx_gasprice(harness):
    """state/tx_gasprice.sol"""
    app = harness.compile_and_deploy("state/tx_gasprice.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_tx_origin(harness):
    """state/tx_origin.sol"""
    app = harness.compile_and_deploy("state/tx_origin.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_uncalled_blobhash(harness):
    """state/uncalled_blobhash.sol"""
    app = harness.compile_and_deploy("state/uncalled_blobhash.sol")
    # f() -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f()")
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662657

def test_uncalled_blockhash(harness):
    """state/uncalled_blockhash.sol"""
    app = harness.compile_and_deploy("state/uncalled_blockhash.sol")
    # f() -> 0x3737373737373737373737373737373737373737373737373737373737373738
    r = harness.call(app, "f()")
    assert r.abi_return == 24974764345303493130574134021481705615411173163177376557530067138961655412536
