"""Empty calls execute receive/fallback and replace the return-data buffer."""

import pytest

from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_lowering_empty_calls(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "lowering_empty_calls", via_ir, profile, slot)
    app = harness.deploy(artifacts, "LoweringEmptyCalls", extra_funding_microalgos=3_000_000)
    assert invoke(harness, app, profile, "zero()", returns=["uint256"] * 3) == (32, 0, 0)
    for literal in (False, True):
        assert invoke(harness, app, profile, "noValue(address,bytes,bool)",
                      [bytes(32), b"", literal], ["bytes", "uint256"]) == (b"", 0)
        assert invoke(harness, app, profile, "run(address,uint256,bytes,bool)",
                      [bytes(32), 0, b"", literal], ["bytes", "uint256"]) == (b"", 0)
    for contract, expected in (("EmptyCallReceiver", b""), ("EmptyCallFallback", b"\x12\x34\x56")):
        target = harness.deploy(artifacts, contract)
        calls = 0
        for literal in (False, True):
            for amount in (None, 0, 1000):
                address = target.app_id.to_bytes(32, "big")
                signature, arguments = ("noValue(address,bytes,bool)", [address, b"", literal]) if amount is None else (
                    "run(address,uint256,bytes,bool)", [address, amount, b"", literal])
                assert invoke(harness, app, profile, signature, arguments,
                              ["bytes", "uint256"]) == (expected, len(expected))
                calls += 1
                assert invoke(harness, target, profile, "calls()") == (calls,)
                assert invoke(harness, target, profile, "received()") == (amount or 0,)
    rejected = harness.deploy(artifacts, "EmptyCallRejecting")
    for literal in (False, True):
        for amount in (0, 1000):
            invoke(harness, app, profile, "run(address,uint256,bytes,bool)",
                   [rejected.app_id.to_bytes(32, "big"), amount, b"", literal], reverts=True)
        invoke(harness, app, profile, "noValue(address,bytes,bool)",
               [rejected.app_id.to_bytes(32, "big"), b"", literal], reverts=True)


def test_empty_payment_clears_account_returndata(harness):
    artifacts = compile_app(harness, "lowering_empty_calls", False, "arc4", False)
    app = harness.deploy(artifacts, "LoweringEmptyCalls", extra_funding_microalgos=1_000_000)
    address = harness.localnet.account.address
    for literal in (False, True):
        for amount in (0, 1000):
            assert invoke(harness, app, "arc4", "run(address,uint256,bytes,bool)",
                          [address, amount, b"", literal], ["bytes", "uint256"]) == (b"", 0)
        assert invoke(harness, app, "arc4", "noValue(address,bytes,bool)",
                      [address, b"", literal], ["bytes", "uint256"]) == (b"", 0)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_asa_operand_order(harness, via_ir, profile):
    artifacts = compile_app(harness, "asa_operand_order", via_ir, profile, False)
    app = harness.deploy(artifacts, "AsaOperandOrder", extra_funding_microalgos=1_000_000)
    assert invoke(harness, app, profile, "run(bool)", [False], ["uint64"]) == (1234 if via_ir else 2341,)
    assert invoke(harness, app, profile, "run(bool)", [True], ["uint64"]) == (1432 if via_ir else 2341,)
