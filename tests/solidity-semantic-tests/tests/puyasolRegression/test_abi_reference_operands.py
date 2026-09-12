"""ABI encoding happens after evaluating operands, including reference mutations."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_abi_reference_operands(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/abi_reference_operands.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    target = harness.deploy(artifacts, "ArrayOperandTarget", fund_wei=20_000_000)
    app = harness.deploy(artifacts, "AbiReferenceOperands", fund_wei=20_000_000)
    address = target.app_id.to_bytes(32, "big")
    assert invoke(harness, app, profile, "externalArgument(address)", [address]) == (19,)
    assert invoke(harness, app, profile, "encodedArgument()") == (19,)
    assert invoke(harness, app, profile, "bareArgument(address)", [address]) == (19,)
