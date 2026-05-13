"""Tests for the interfaceID category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_homer(harness):
    """interfaceID/contracts/homer.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/homer.sol")
    # supportsInterface(bytes4): left(0x01ffc9a0) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a000000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False
    # supportsInterface(bytes4): left(0x01ffc9a7) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a700000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x73b6b492) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x73b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x70b6b492) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x70b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False

def test_homer_interfaceId(harness):
    """interfaceID/contracts/homer_interfaceId.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/homer_interfaceId.sol")
    # supportsInterface(bytes4): left(0x01ffc9a0) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a000000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False
    # supportsInterface(bytes4): left(0x01ffc9a7) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a700000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x73b6b492) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x73b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x70b6b492) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x70b6b49200000000000000000000000000000000000000000000000000000000)
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
    # supportsInterface(bytes4): left(0x01ffc9a0) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a000000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False
    # supportsInterface(bytes4): left(0x01ffc9a7) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a700000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x73b6b492) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x73b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x70b6b492) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x70b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False

def test_lisa_interfaceId(harness):
    """interfaceID/contracts/lisa_interfaceId.sol"""
    app = harness.compile_and_deploy("interfaceID/contracts/lisa_interfaceId.sol")
    # supportsInterface(bytes4): left(0x01ffc9a0) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a000000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False
    # supportsInterface(bytes4): left(0x01ffc9a7) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a700000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x73b6b492) -> true
    r = harness.call(app, "supportsInterface(bytes4)", 0x73b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is True
    # supportsInterface(bytes4): left(0x70b6b492) -> false
    r = harness.call(app, "supportsInterface(bytes4)", 0x70b6b49200000000000000000000000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False
