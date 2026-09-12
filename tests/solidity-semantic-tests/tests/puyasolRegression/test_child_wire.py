"""Both constructor entries consume the same source values and planned wire types."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_child_wire(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/child_wire.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "ChildWireFactory", fund_wei=60_000_000, postinit_budget_pool=8)
    for method in ("inlineValues()", "deferredValues()"):
        assert invoke(harness, app, profile, method,
                      returns=["uint256", "uint256", "bool", "bytes4"]) == (1212, 2**100, True, bytes.fromhex("12345678"))
