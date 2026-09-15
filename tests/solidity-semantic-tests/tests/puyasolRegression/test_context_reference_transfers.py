"""Solc-resolved storage offsets and tuple memory reference transfers."""

import pytest
from framework import as_signed_int
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_context_reference_transfers(harness, via_ir, profile, slot):
    artifacts = harness.compile(
        "puyasolRegression/contracts/context_reference_transfers.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []),
    )
    for contract, expected in [("ContextOffsetBase", 1), ("ContextOffsetDerived", 9)]:
        app = harness.deploy(artifacts, contract, fund_wei=20_000_000)
        assert invoke(harness, app, profile, "check()", returns=["uint256"] * 2) == (0, expected)
        if contract == "ContextOffsetDerived":
            assert invoke(harness, app, profile, "checkBase()", returns=["uint256"] * 2) == (0, 1)
    app = harness.deploy(artifacts, "ContextMemoryTransfers", fund_wei=20_000_000)
    for method, expected in [("assigned", 9), ("declared", 11), ("hole", 13)]:
        assert invoke(harness, app, profile, method + "()") == (expected,), method
    assert invoke(harness, app, profile, "swap()", returns=["uint256"] * 2) == (2, 17)
    assert invoke(harness, app, profile, "branch(bool)", [True], ["uint256"] * 2) == (19, 2)
    assert invoke(harness, app, profile, "branch(bool)", [False], ["uint256"] * 2) == (1, 19)
    x, y = invoke(harness, app, profile, "narrow(uint8,int8)", [254, -126], ["uint8", "int8"])
    assert (x, as_signed_int(y)) == (255, -127)
    assert invoke(harness, app, profile, "copyToStorage()", returns=["uint256"] * 2) == (1, 23)
