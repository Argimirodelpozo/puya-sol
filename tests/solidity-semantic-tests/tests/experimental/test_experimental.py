"""Tests for the experimental category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

@pytest.mark.xfail(reason="OUT OF SCOPE: `pragma experimental solidity` selects solc's experimental NEXT-GENERATION language (word builtins, different frontend) — a separate language, not a Solidity feature.", strict=False)
def test_stub(harness):
    """experimental/contracts/stub.sol"""
    app = harness.compile_and_deploy('experimental/contracts/stub.sol')

@pytest.mark.xfail(reason="uses Solidity `pragma experimental` features (type class) — experimental Solidity is not supported on AVM", strict=False)
def test_type_class(harness):  # currently fails
    """experimental/contracts/type_class.sol"""
    app = harness.compile_and_deploy('experimental/contracts/type_class.sol')
