"""Frame identity and member evaluation, checked against solc legacy and IR."""

import pytest
from test_call_operands import invoke
from test_root_inventory import compile_source


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_sol_ast_boundary_facts(harness, via_ir, profile):
    artifacts = harness.compile("puyasolRegression/contracts/sol_ast_boundary_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "SolAstBoundaryFacts", fund_wei=20_000_000)
    for name in ("frame", "inheritedFrame"):
        assert invoke(harness, app, profile, name + "()") == (7,), name
    assert invoke(harness, app, profile, "fields()", returns=["uint64"] * 4) == (17, 23, 3, 4)
    for name in ("addressOptions", "selectorOptions"):
        assert invoke(harness, app, profile, name + "()", returns=["bool", "uint64"]) == (True, 1)
    for choose in (False, True):
        assert invoke(harness, app, profile, "conditionalAddress(bool)", [choose],
                      ["bool", "uint64"]) == (True, 1 if choose else 2)
    assert invoke(harness, app, profile, "unusedOptions()", returns=["uint64"]) == (1,)


def test_public_memory_frames_have_entry_bindings(tmp_path):
    result, _, _ = compile_source(tmp_path, """
        contract C {
            struct Value { uint256 x; }
            function mutate(Value memory value) public pure {
                value.x = 7; value = Value(9); value.x = 11;
            }
            function read() external pure returns(uint256) {
                Value memory value = Value(5); mutate(value); return value.x;
            }
        }
    """, backend=True)
    diagnostics = result.stdout + result.stderr
    assert result.returncode == 0, diagnostics
    assert "before assignment" not in diagnostics, diagnostics
