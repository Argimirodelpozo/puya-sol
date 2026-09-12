"""Creation effects must not inherit unrelated deployed/overridden bodies."""

import json
import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_creation_facts(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/creation_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    cases = [("CreationRuntimeOnly", 3, slot), ("CreationOverride", 7, slot),
             ("CreationInheritedArg", 9, True), ("CreationModifier", 10, True),
             ("CreationSelf", 11, True), ("CreationMemoryOnly", 3, slot),
             ("CreationNative", None, True)]
    for name, expected, deferred in cases:
        spec = json.loads(artifacts.by_contract[name]["arc56"].read_text())
        assert any(method["name"] == "__postInit" for method in spec["methods"]) == deferred, name
        app = harness.deploy(artifacts, name, fund_wei=20_000_000, postinit_budget_pool=8)
        assert invoke(harness, app, profile, "value()", returns=["uint64"]) == (
            app.app_id if expected is None else expected,)
