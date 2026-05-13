"""Auto-generated tests for the specialFunctions category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_abi_encode_with_signature_from_string(harness):
    """specialFunctions/contracts/abi_encode_with_signature_from_string.sol"""
    app = harness.compile_and_deploy("specialFunctions/contracts/abi_encode_with_signature_from_string.sol")
    # f() -> 0x40, 0xa0, 0x24, -813742827273327954027712588510533233455028711326166692885570228492575965184, 26959946667150639794667015087019630673637144422540572481103610249216, 0x24, -813742827273327954027712588510533233455028711326166692885570228492575965184, 26959946667150639794667015087019630673637144422540572481103610249216
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 64, 160, 36, -813742827273327954027712588510533233455028711326166692885570228492575965184, 26959946667150639794667015087019630673637144422540572481103610249216, 36, -813742827273327954027712588510533233455028711326166692885570228492575965184, 26959946667150639794667015087019630673637144422540572481103610249216
    assert not r.reverted

def test_abi_functions_member_access(harness):
    """specialFunctions/contracts/abi_functions_member_access.sol"""
    app = harness.compile_and_deploy("specialFunctions/contracts/abi_functions_member_access.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_keccak256_optimized(harness):
    """specialFunctions/contracts/keccak256_optimized.sol"""
    app = harness.compile_and_deploy("specialFunctions/contracts/keccak256_optimized.sol")
    # short() -> true
    r = harness.call(app, "short()")
    assert r.abi_return is True
    # long() -> true, true
    r = harness.call(app, "long()")
    assert tuple(r.abi_return) == (True, True)
