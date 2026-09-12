"""Reference identity, conditional effects, fresh values and parameter rebinding."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_modifier_reference_facts(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy("puyasolRegression/contracts/modifier_memory_rebind.sol",
                                    via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", profile]
                                    + (["--evm-storage-layout"] if slot else []))
    for method, expected in (("callG()", 1), ("callH()", 12012), ("callArray()", 12012),
                              ("callK()", 2002 * 10000 + 2002)):
        assert invoke(harness, app, profile, method) == (expected,)
    for mode in range(8):
        assert invoke(harness, app, profile, "callN(uint256)", [mode], ["uint256"] * 4) == (
            (2, 10, 2, 10) if mode < 4 else (1, 11, 1, 11))
    for existing in (False, True):
        assert invoke(harness, app, profile, "callMixed(bool)", [existing], ["uint256"] * 3) == (
            (11, 11, 0) if existing else (1, 19, 1))
