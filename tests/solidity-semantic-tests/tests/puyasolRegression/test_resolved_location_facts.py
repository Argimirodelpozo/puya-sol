"""Resolved addresses survive wrappers and delayed aggregate write-back."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_resolved_location_facts(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy("puyasolRegression/contracts/resolved_location_facts.sol",
                                    via_yul_behavior=via_ir, fund_wei=20_000_000,
                                    extra_args=["--contract-abi", profile]
                                    + (["--evm-storage-layout"] if slot else []))
    assert invoke(harness, app, profile, "delayedWriteback()", returns=["uint256"] * 2) == (7, 9)
    assert invoke(harness, app, profile, "frozenTupleIndex()", returns=["uint256"] * 3) == (7, 4, 1)
    for choose in (False, True):
        assert invoke(harness, app, profile, "conditionalBlob(bool)", [choose], ["uint256"] * 2) == (
            (7, 2) if choose else (1, 7))
    assert invoke(harness, app, profile, "parenthesizedBlob()") == (8,)
    assert invoke(harness, app, profile, "castBlob()", returns=["bytes1"]) == (b"\x33",)
