"""Reference mutation/rebinding expectations checked against solc legacy and IR."""

import pytest
from Crypto.Hash import keccak
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_reference_boundary_identity(harness, via_ir, profile):
    artifacts = harness.compile("puyasolRegression/contracts/reference_boundary_identity.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "ReferenceBoundaryIdentity", fund_wei=20_000_000)
    for name, expected in {
        "viaInternal": 9, "viaPrivate": 9, "viaPublic": 9, "viaAlias": 9,
        "viaFree": 9, "viaLibrary": 9, "viaLibraryRebind": 7, "viaRebind": 5,
        "viaMutateRebind": 7, "viaAliasRebind": 13, "viaTupleRebind": 7,
        "viaLoopRebind": 7, "viaArrayRebind": 7,
    }.items():
        assert invoke(harness, app, profile, name + "()", returns=["uint256"]) == (expected,), name
    assert invoke(harness, app, profile, "viaConditional(bool)", [False], ["uint256"] * 2) == (21, 21)
    assert invoke(harness, app, profile, "viaConditional(bool)", [True], ["uint256"] * 2) == (5, 21)
    assert invoke(harness, app, profile, "viaReturningCall()", returns=["uint256"] * 2) == (42, 42)
    assert invoke(harness, app, profile, "localRebind()", returns=["uint256"] * 2) == (9, 7)
    assert invoke(harness, app, profile, "emptyHash()", returns=["bytes32"]) == (bytes(32),)
    expected = keccak.new(digest_bits=256, data=(1).to_bytes(32, "big") + (2).to_bytes(32, "big")).digest()
    assert invoke(harness, app, profile, "actualHash()", returns=["bytes32"]) == (expected,)
