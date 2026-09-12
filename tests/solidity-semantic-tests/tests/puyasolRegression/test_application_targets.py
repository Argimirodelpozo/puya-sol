"""Explicit AVM app-id/self namespace; non-application addresses must not alias apps."""

import pytest
from Crypto.Hash import keccak
from algosdk.encoding import decode_address
from algosdk.logic import get_application_address
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_application_targets(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/application_targets.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    target = harness.deploy(artifacts, "ApplicationTargetProbe", fund_wei=20_000_000)
    app = harness.deploy(artifacts, "ApplicationTargets", fund_wei=20_000_000)
    assert invoke(harness, app, profile, "constructionSize()") == (0,)
    encoded = target.app_id.to_bytes(32, "big")
    self_address = decode_address(get_application_address(app.app_id))
    bad_prefix = (target.app_id + 2**128).to_bytes(32, "big")
    absent = (2**64 - 1).to_bytes(32, "big")
    for method in ("typed(address)", "bare(address)", "staticTarget(address)"):
        assert invoke(harness, app, profile, method, [encoded], ["uint64"]) == (7,)
        # AVM rejects runtime self inner transactions; only statically resolved
        # self calls have the existing direct-subroutine adaptation.
        for invalid in (bytes(32), bad_prefix, absent, self_address):
            invoke(harness, app, profile, method, [invalid], reverts=True)
    for address in (encoded, self_address):
        high = invoke(harness, app, profile, "size(address)", [address])
        assert high[0] > 0
        assert invoke(harness, app, profile, "yulSize(address)", [address]) == high
    for size in (0, 1, 3, 4, 5):
        digest = keccak.new(digest_bits=256, data=bytes.fromhex("123456789a")[:size]).digest()
        assert invoke(harness, app, profile, "yulRaw(address,uint256)", [encoded, size],
                      ["uint256"] * 3) == (64, size, int.from_bytes(digest, "big"))
    for invalid in (bytes(32), bad_prefix, absent, self_address):
        invoke(harness, app, profile, "yulRaw(address,uint256)", [invalid, 4], reverts=True)
    for invalid in (bytes(32), bad_prefix, absent):
        assert invoke(harness, app, profile, "size(address)", [invalid]) == (0,)
        assert invoke(harness, app, profile, "yulSize(address)", [invalid]) == (0,)
