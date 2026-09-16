"""Grouping must not change lowering; real tuples/inline arrays remain values."""

from pathlib import Path

import pytest

from test_call_operands import invoke


@pytest.mark.parametrize("depth", [0, 1, 3], ids=["plain", "grouped", "nested"])
@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_parenthesized_expressions(harness, tmp_path, depth, via_ir, profile, slot):
    template = Path(__file__).parent / "contracts" / "parenthesized_expressions.sol"
    source = tmp_path / template.name
    source.write_text(template.read_text().replace("/*(*/", "(" * depth).replace("/*)*/", ")" * depth))
    artifacts = harness.compile(source, via_yul_behavior=via_ir,
                                extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "ParenthesizedExpressions", fund_wei=30_000_000,
                         postinit_budget_pool=8)
    for choose, expected in ((False, 23), (True, 12)):
        assert invoke(harness, app, profile, "slotReferences(bool)", [choose],
                      ["uint256"] * 2) == (expected, 33)
    assert invoke(harness, app, profile, "transientValue()", returns=["uint256"] * 2) == (8, 0)
    assert invoke(harness, app, profile, "namedArrays()", returns=["uint256"] * 2) == (7, 9)
    assert invoke(harness, app, profile, "constants()", returns=["bytes4", "bytes4", "bytes20"]) == (
        bytes.fromhex("01020304"), bytes.fromhex("01020304"),
        bytes.fromhex("9c1185a5c5e9fc54612808977ee8f548b2258d31"))
    assert invoke(harness, app, profile, "functionValues()", returns=["uint256", "bool"]) == (8, True)
    assert invoke(harness, app, profile, "effects()", returns=["uint256"] * 2) == (41, 1)
    assert invoke(harness, app, profile, "customRequire(bool)", [True]) == (1,)
    invoke(harness, app, profile, "customRequire(bool)", [False], reverts=True)
    assert invoke(harness, app, profile, "tuplesAndArrays()", returns=["uint256"] * 3) == (3, 3, 7)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_parenthesized_custom_error(harness, tmp_path, via_ir, profile, slot):
    # Pinned solc accepts this AST but its own code generators also bad_cast on
    # the grouped error. The plain form supplies the independent EVM oracle;
    # this test checks our grouping boundary, eager operands and failing branch.
    source = tmp_path / "CustomError.sol"
    source.write_text("""pragma solidity ^0.8.28;
        contract CustomError {
            error Failure(uint256 count);
            uint256 calls;
            function next() internal returns (uint256) { return calls++; }
            function check(bool ok) external returns (uint256) {
                calls = 0;
                require(ok, (((Failure(((next())))))));
                return calls;
            }
        }
    """)
    artifacts = harness.compile(source, via_yul_behavior=via_ir,
                                extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "CustomError")
    assert invoke(harness, app, profile, "check(bool)", [True]) == (1,)
    invoke(harness, app, profile, "check(bool)", [False], reverts=True)
