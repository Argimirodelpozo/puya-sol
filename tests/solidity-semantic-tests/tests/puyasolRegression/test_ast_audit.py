"""AST normalization, reference bounds and tuple copying: solc legacy/IR oracles."""

import pytest
from test_call_operands import invoke
from test_root_inventory import compile_source


def compile_app(harness, source, via_ir, profile, slot):
    return harness.compile("puyasolRegression/contracts/" + source + ".sol",
                           via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                           + (["--evm-storage-layout"] if slot else []))


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_ast_expression_facts(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "ast_expression_facts", via_ir, profile, slot)
    app = harness.deploy(artifacts, "AstExpressionFacts", fund_wei=30_000_000)
    target = harness.deploy(artifacts, "AstOptionsTarget", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "singleton()") == (7,)
    for choose in (False, True):
        assert invoke(harness, app, profile, "addresses(bool)", [choose], ["bool"]) == (True,)
        assert invoke(harness, app, profile, "options(address,bool)",
                      [target.app_id.to_bytes(32, "big"), choose], ["uint64", "uint256"]) == (12, 7)
    for receiver in (0, target.app_id):
        assert invoke(harness, app, profile, "foreign(address)",
                      [receiver.to_bytes(32, "big")], ["bool"]) == (True,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_ast_reference_bounds(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "ast_reference_bounds", via_ir, profile, slot)
    app = harness.deploy(artifacts, "AstReferenceBounds", fund_wei=30_000_000, postinit_budget_pool=8)
    for name, expected in (("element", 123), ("dynamicElement", 124)):
        assert invoke(harness, app, profile, name + "(uint256)", [0]) == (expected,)
        for i in (1, 2, 2**64, 2**256 - 1):
            invoke(harness, app, profile, name + "(uint256)", [i], reverts=True)
    assert invoke(harness, app, profile, "nested(uint256,uint256)", [0, 0]) == (123,)
    for indices in ([0, 1], [1, 0], [2**64, 0], [0, 2**64]):
        invoke(harness, app, profile, "nested(uint256,uint256)", indices, reverts=True)
    assert invoke(harness, app, profile, "growing()") == (123,)
    assert invoke(harness, app, profile, "calldataPointer(uint256[2][],uint256)", [[[11, 22]], 0]) == (100,)
    for i in (1, 2, 2**64, 2**256 - 1):
        invoke(harness, app, profile, "calldataPointer(uint256[2][],uint256)", [[[11, 22]], i], reverts=True)
    assert invoke(harness, app, profile, "calldataLocal(uint256[2][],uint256)", [[[11, 22]], 0]) == (100,)
    invoke(harness, app, profile, "calldataLocal(uint256[2][],uint256)", [[[11, 22]], 1], reverts=True)
    assert invoke(harness, app, profile, "calldataFixed(uint256[2][2],uint256)", [[[11, 22], [33, 44]], 1]) == (68,)
    for i in (2, 2**64, 2**256 - 1):
        invoke(harness, app, profile, "calldataFixed(uint256[2][2],uint256)", [[[11, 22], [33, 44]], i], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_ast_tuple_storage(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "ast_tuple_storage", via_ir, profile, slot)
    app = harness.deploy(artifacts, "AstTupleStorage", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "dynamicMembers()", returns=["uint256", "uint256", "bytes", "bytes"]) == (
        11, 11, b"\x01\x02\x03", b"\x01\x02\x03")
    assert invoke(harness, app, profile, "packedMembers()", returns=["bool"]) == (True,)
    assert invoke(harness, app, profile, "parenthesizedDestinations()", returns=["uint256"] * 3) == (7, 9, 2)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_ast_external_override_data_location(harness, via_ir, profile, slot):
    artifacts = harness.compile("inheritance/contracts/inherited_function_calldata_memory.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "B")
    assert invoke(harness, app, profile, "g()") == (23,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_ast_inherited_constructor_array_alias(harness, via_ir, profile, slot):
    artifacts = harness.compile("types/contracts/array_mapping_abstract_constructor_param.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "C")
    assert invoke(harness, app, profile, "m(uint256,uint256,uint256)", [1, 0, 1]) == (2,)
    assert invoke(harness, app, profile, "m(uint256,uint256,uint256)", [1, 0, 5]) == (0,)


@pytest.mark.parametrize("parentheses", [0, 1, 2])
@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_create2_options_cannot_be_hidden(tmp_path, parentheses, via_ir):
    callee = "(" * parentheses + "new Child{salt: bytes32(0)}" + ")" * parentheses
    result, _, _ = compile_source(tmp_path, "contract Child {} contract C { function create() external { "
                                 + callee + "(); } }", extra=["--via-yul-behavior"] if via_ir else [])
    assert result.returncode != 0
    assert "CREATE2" in result.stderr
