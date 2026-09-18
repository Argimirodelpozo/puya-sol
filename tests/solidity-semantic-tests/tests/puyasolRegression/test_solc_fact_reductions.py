"""Reference-return identities and cached solc facts; expectations checked against solc."""

import pytest

from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_returned_memory_identity(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "returned_memory_identity", via_ir, profile, slot)
    app = harness.deploy(artifacts, "ReturnedMemoryIdentity", fund_wei=30_000_000)
    for name, expected in (
        ("direct", (9,)),
        ("throughReturn", (9,)),
        ("retainedAlias", (7, 9)),
        ("localReturnedAlias", (9, 9)),
        ("duplicateReturn", (9, 9, 9)),
        ("defaults", (9, 0, 7, 0)),
        ("libraryAlias", (9,)),
        ("libraryCopy", (5, 7, 9)),
        ("libraryPairCopy", (5, 9, 5)),
        ("publicAlias", (9,)),
        ("publicPointer", (9, 9, 5)),
        ("directDestination", (9, 1)),
        ("argumentOnce", (9, 1)),
        ("modifierAlias", (9, 9)),
    ):
        assert invoke(harness, app, profile, name + "()", returns=["uint256"] * len(expected)) == expected
    for flag in (False, True):
        assert invoke(harness, app, profile, "conditionalFreshTuple(bool)", [flag], ["uint256"] * 2) == (
            (9, 9) if flag else (5, 8))
        assert invoke(harness, app, profile, "conditionalTuple(bool)", [flag], ["uint256"] * 3) == (
            (9, 7, 9) if flag else (5, 9, 9))
        assert invoke(harness, app, profile, "conditional(bool)", [flag], ["uint256"] * 2) == (
            9 if flag else 5, 9)
        assert invoke(harness, app, profile, "dispatch(bool)", [flag], ["uint256"] * 2) == (
            (9, 7) if flag else (5, 9))
    assert invoke(harness, app, profile, "publicIdentity((uint256))", [[17]], ["(uint256)"]) == (
        ((17,),) if profile == "evm" else (17,))  # ARC4 helper unwraps singleton structs.
    app = harness.deploy(artifacts, "ReturnedMemoryArrays", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "arrays()", returns=["uint256"] * 3) == (9, 9, 7)
    assert invoke(harness, app, profile, "nested()") == (9,)
    assert invoke(harness, app, profile, "bytesAlias()", returns=["bytes"]) == (b"\x01\xff\x03",)
