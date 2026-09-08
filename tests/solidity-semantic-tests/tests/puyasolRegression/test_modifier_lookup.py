"""solc's static/virtual modifier lookup across an inheritance diamond."""

import pytest

from framework import as_int


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_modifier_lookup(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_lookup.sol",
        contract_name="ModifierLookup",
        via_yul_behavior=via_ir,
    )
    for method, result, trace in (
        ("inherited()", 7, 78),
        ("explicitBase()", 1, 12),
        ("explicitLeft()", 3, 34),
        ("stacked()", 17, 1782),
    ):
        harness.call(app, "reset()")
        assert as_int(harness.call(app, method).abi_return) == result
        assert as_int(harness.call(app, "trace()").abi_return) == trace
