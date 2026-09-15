"""Solc-oracled regressions for the experimental sharing fixed point."""

import pytest

from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("fixture", ["returns", "hosts", "constructors", "raw_yul", "unique"])
def test_memory_sharing(harness, via_ir, profile, fixture):
    artifacts = harness.compile(
        "puyasolRegression/contracts/sharing_" + fixture + ".sol",
        via_yul_behavior=via_ir,
        extra_args=["--memory-model", "scratch", "--contract-abi", profile],
    )
    probes = {
        "returns": {"SharingReturns": [
            ("namedAlias()", [], (17,)), ("branchAlias(bool)", [False], (19,)),
            ("branchAlias(bool)", [True], (19,)), ("returnedArgument()", [], (23,)),
            ("returnedMember()", [], (29,)),
        ]},
        "hosts": {"SharingBase": [("check()", [], (31,))],
                  "SharingDerived": [("check()", [], (37,))]},
        "constructors": {"SharingConstructors": [
            ("constructorAlias()", [], (41, 1)), ("nestedConstructor()", [], (47, 43)),
            ("modifierAlias()", [], (59,)),
        ]},
        "raw_yul": {"SharingRawYul": [
            ("readUnnamedObject()", [], (61,)), ("writeUnnamedObject()", [], (67,)),
            ("constructorOrder()", [], (71, 0 if via_ir else 71, 128)),
        ]},
        "unique": {"SharingUnique": [("check(uint256)", [73], (73,))]},
    }
    for contract, calls in probes[fixture].items():
        app = harness.deploy(artifacts, contract, fund_wei=20_000_000)
        for signature, args, expected in calls:
            assert invoke(harness, app, profile, signature, args,
                          ["uint256"] * len(expected)) == expected, (contract, signature, args)
