"""Yul words, operand capture and memory range boundaries; solc is the oracle."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_assembly_refactors(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/assembly_refactors.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    def check(signature, arguments=(), returns=("uint256",)):
        return invoke(harness, app, profile, signature, arguments, returns)
    for value in (0, 1, 2**63, 2**64 - 1):
        assert check("unsignedSigns(uint64)", [value], ["uint256"] * 3) == (0, int(value > 0), int(value > 0))
    for value in (-(2**63), -1, 0, 1, 2**63 - 1):
        assert check("signedSigns(int64)", [value], ["uint256"] * 3) == (int(value < 0), int(value > 0), int(value > 0))
    assert check("ordered(uint256)", [2], ["uint256"] * 2) == (3, 10)
    assert check("statementOrder(uint256)", [2]) == (2,)
    assert check("addressSpaces(uint256)", [31], ["uint256"] * 2) == (7, 31)
    assert check("fullWidth()") == (5,)
    for offset in (2**64, 2**255, 2**256 - 1):
        assert check("farCalldata(uint256)", [offset]) == (0,)
        assert check("emptyCopy(uint256)", [offset]) == (1,)
        invoke(harness, app, profile, "wordAt(uint256)", [offset], reverts=True)
    for offset in (4090, 4096, 5 * 4096 - 32):
        assert check("wordAt(uint256)", [offset]) == (123,)
    invoke(harness, app, profile, "wordAt(uint256)", [5 * 4096 - 31], reverts=True)
    for size in (0, 1, 31, 32, 33):
        assert check("partialCopy(uint256,uint256)", [4090, size]) == (
            int.from_bytes(b"\xff" * min(size, 32) + bytes(32 - min(size, 32)), "big"),)
