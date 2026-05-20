"""puya-sol semantic test framework.

Public API:
    Harness                — high-level fixture: compile + deploy + call.
    Result                 — typed call result (abi_return, logs, reverted).
    lpad / rpad / u256 ... — explicit Solidity-style value helpers.
    Reverted, Panic, Error — revert classifiers used in expectations.

Pytest usage:
    def test_foo(harness):
        app = harness.deploy("path/to/foo.sol")
        assert harness.call(app, "f()").abi_return == 42
"""

# Patch algosdk to (a) parse int<N> as a signed-int ABI type and (b) encode
# negative Python ints as two's complement. Must run before any algosdk ABI
# import in user code — the framework re-exports everything, so this is
# safe.
from . import _algosdk_patch  # noqa: F401

from .harness import Harness, Result, App
from .values import lpad, rpad, u256, i256, hex_bytes, str_bytes, raw, as_int, as_signed_int, as_bytes
from .revert import Reverted, Panic, ErrorString, RawRevert

__all__ = [
    "Harness",
    "Result",
    "App",
    "lpad",
    "rpad",
    "u256",
    "i256",
    "hex_bytes",
    "str_bytes",
    "raw",
    "as_int",
    "as_signed_int",
    "as_bytes",
    "Reverted",
    "Panic",
    "ErrorString",
    "RawRevert",
]
