"""Auto-generated tests for the metaTypes category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_name_other_contract(harness):
    """metaTypes/contracts/name_other_contract.sol"""
    app = harness.compile_and_deploy("metaTypes/contracts/name_other_contract.sol")
    # c() -> 0x20, 1, "C"
    r = harness.call(app, "c()")
    assert r.abi_return == 'C'
    # a() -> 0x20, 1, "A"
    r = harness.call(app, "a()")
    assert r.abi_return == 'A'
    # i() -> 0x20, 1, "I"
    r = harness.call(app, "i()")
    assert r.abi_return == 'I'
