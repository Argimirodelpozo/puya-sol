"""Child creation uses backend state totals, including more than 16 cells."""

import json
import pytest

from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_child_schema(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/child_schema.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    child = artifacts.by_contract["LargeChild"]
    spec = json.loads(child["arc56"].read_text())
    template = json.loads((child["arc56"].parent / "deploy.tmpl.json").read_text())
    for scope in ("global", "local"):
        for kind, suffix in (("ints", "NumUint"), ("bytes", "NumByteSlice")):
            assert template[f"TMPL_CHILD_LargeChild_{scope.title()}{suffix}"] == spec["state"]["schema"][scope][kind]
    if not slot:
        assert template["TMPL_CHILD_LargeChild_GlobalNumUint"] == 17
    app = harness.deploy(artifacts, "ChildFactory", fund_wei=40_000_000, postinit_budget_pool=8)
    assert invoke(harness, app, profile, "make()", returns=["uint64"]) == (17,)
