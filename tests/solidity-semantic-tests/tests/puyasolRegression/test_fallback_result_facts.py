"""Void method/receive/fallback events are not return data, for either transport."""

import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_fallback_result_facts(harness, via_ir, profile):
    artifacts = harness.compile("puyasolRegression/contracts/fallback_result_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "FallbackResultFacts")
    target = harness.deploy(artifacts, "EventOnlyFallback")
    address = target.app_id.to_bytes(32, "big")
    for data in (b"", b"a", b"abc", bytes.fromhex("deadbeef"), bytes.fromhex("deadbeef0102030405")):
        assert invoke(harness, app, profile, "run(address,bytes)", [address, data], ["bytes", "uint256"]) == (b"", 0)
        assert invoke(harness, target, profile, "marker()", returns=["uint64"]) == ((1,) if not data else (2,))
    for method in ("typed", "pointer"):
        assert invoke(harness, app, profile, method + "(address)", [address]) == (0,)
        assert invoke(harness, target, profile, "marker()", returns=["uint64"]) == (3,)
    assert invoke(harness, app, profile, "encoded(address)", [address], ["bytes", "uint256"]) == (b"", 0)
    assert invoke(harness, target, profile, "marker()", returns=["uint64"]) == (3,)
    assert invoke(harness, app, profile, "typedRaw(address)", [address]) == (32,)
