"""Tests for the interfaceID category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes, arc4_selector,
)

def _xor4(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))

# EVM_DIVERGENCE: ERC-165 IDs are XORs of sha512_256 selectors on AVM
# (keccak on EVM) — matches type(I).interfaceId and f.selector on-chain.
ERC165_ID = arc4_selector("supportsInterface(byte[4])bool")
SIMPSON_ID = _xor4(arc4_selector("is2D()bool"), arc4_selector("skinColor()string"))
def _not_an_id(idb: bytes) -> bytes:
    return bytes([idb[0] ^ 0x07]) + idb[1:]


def test_homer(harness):
    """interfaceID/contracts/homer.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/homer.sol")
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(ERC165_ID))
    assert bool(as_int(r.abi_return)) is False
    r = harness.call(app, "supportsInterface(bytes4)", ERC165_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", SIMPSON_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(SIMPSON_ID))
    assert bool(as_int(r.abi_return)) is False

def test_homer_interfaceId(harness):
    """interfaceID/contracts/homer_interfaceId.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/homer_interfaceId.sol")
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(ERC165_ID))
    assert bool(as_int(r.abi_return)) is False
    r = harness.call(app, "supportsInterface(bytes4)", ERC165_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", SIMPSON_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(SIMPSON_ID))
    assert bool(as_int(r.abi_return)) is False

def test_interfaceId_events(harness):
    """interfaceID/contracts/interfaceId_events.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/interfaceId_events.sol")
    # hello_world() -> left(0xc6be8b58)
    r = harness.call(app, "hello_world()")
    # TODO: verify expected: left(0xc6be8b58)
    assert not r.reverted
    # hello_world_with_event() -> left(0xc6be8b58)
    r = harness.call(app, "hello_world_with_event()")
    # TODO: verify expected: left(0xc6be8b58)
    assert not r.reverted

def test_interfaces(harness):
    """interfaceID/contracts/interfaces.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/interfaces.sol")
    # hello() -> left(0x19ff1d21)
    r = harness.call(app, "hello()")
    # TODO: verify expected: left(0x19ff1d21)
    assert not r.reverted
    # world() -> left(0xdf419679)
    r = harness.call(app, "world()")
    # TODO: verify expected: left(0xdf419679)
    assert not r.reverted
    # ERC165_interfaceId() -> left(0x01ffc9a7)
    r = harness.call(app, "ERC165_interfaceId()")
    # TODO: verify expected: left(0x01ffc9a7)
    assert not r.reverted
    # hello_world() -> left(0xc6be8b58)
    r = harness.call(app, "hello_world()")
    # TODO: verify expected: left(0xc6be8b58)
    assert not r.reverted
    # hello_world_interfaceId() -> left(0xc6be8b58)
    r = harness.call(app, "hello_world_interfaceId()")
    # TODO: verify expected: left(0xc6be8b58)
    assert not r.reverted
    # ghello_world_interfaceId() -> left(0xc6be8b58)
    r = harness.call(app, "ghello_world_interfaceId()")
    # TODO: verify expected: left(0xc6be8b58)
    assert not r.reverted
    # other() -> left(0x85295877)
    r = harness.call(app, "other()")
    # TODO: verify expected: left(0x85295877)
    assert not r.reverted
    # hello_world_derived_interfaceId() -> left(0x85295877)
    r = harness.call(app, "hello_world_derived_interfaceId()")
    # TODO: verify expected: left(0x85295877)
    assert not r.reverted

def test_lisa(harness):
    """interfaceID/contracts/lisa.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/lisa.sol")
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(ERC165_ID))
    assert bool(as_int(r.abi_return)) is False
    r = harness.call(app, "supportsInterface(bytes4)", ERC165_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", SIMPSON_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(SIMPSON_ID))
    assert bool(as_int(r.abi_return)) is False

def test_lisa_interfaceId(harness):
    """interfaceID/contracts/lisa_interfaceId.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/lisa_interfaceId.sol")
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(ERC165_ID))
    assert bool(as_int(r.abi_return)) is False
    r = harness.call(app, "supportsInterface(bytes4)", ERC165_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", SIMPSON_ID)
    assert bool(as_int(r.abi_return)) is True
    r = harness.call(app, "supportsInterface(bytes4)", _not_an_id(SIMPSON_ID))
    assert bool(as_int(r.abi_return)) is False
