"""Solc-bound storage aliases and transitive inline-assembly reference effects."""

import pytest
from framework.compile import CompileError
from test_call_operands import invoke
from test_root_inventory import compile_source


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_storage_alias_facts(harness, via_ir, profile, slot):
    if not slot:
        with pytest.raises(CompileError, match="--evm-storage-layout"):
            harness.compile("puyasolRegression/contracts/storage_alias_facts.sol",
                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
        return
    artifacts = harness.compile("puyasolRegression/contracts/storage_alias_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "StorageAliasFacts", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "named()", returns=["uint64"] * 2) == (21 if via_ir else 12, 5)
    assert invoke(harness, app, profile, "bound()", returns=["uint64"] * 2) == (321 if via_ir else 123, 5)
    assert invoke(harness, app, profile, "virtualTarget()", returns=["uint64"] * 2) == (3, 6)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_transitive_assembly_effects(harness, via_ir, profile):
    artifacts = harness.compile("puyasolRegression/contracts/transitive_assembly_effects.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "TransitiveAssemblyEffects", fund_wei=20_000_000)
    for name, ir_result in {"viaDirect": 8, "viaWrapper": 10, "viaRecursion": 10, "viaModifier": 12}.items():
        assert invoke(harness, app, profile, name + "()") == (ir_result if via_ir else 6,), name
    assert invoke(harness, app, profile, "viaAlias()", returns=["uint256"] * 2) == (6 if via_ir else 10, 9)


@pytest.mark.parametrize("reason", ["modifier", "different_element", "different_length"])
def test_pointer_cast_body_is_not_elided_without_proof(tmp_path, reason):
    # Compiler-only checks: bodies outside the exact identity proof must remain
    # calls. No storage type-punning operation is executed by this test.
    field = "uint16[2]" if reason == "different_element" else "uint8[3]" if reason == "different_length" else "uint8[2]"
    modifier = "counted" if reason == "modifier" else ""
    mutability = "" if modifier else "pure"
    source = f"""
        contract C {{
            struct Wrapper {{ {field} value; }}
            uint8[2] private data;
            uint64 private count;
            modifier counted() {{ ++count; _; }}
            function pointer(uint8[2] storage source) internal {mutability} {modifier}
                returns (Wrapper storage result) {{ assembly {{ result.slot := source.slot }} }}
            function read() external returns (uint256) {{ return pointer(data).value[0]; }}
        }}
    """
    result, roots, _ = compile_source(tmp_path, source, extra=["--evm-storage-layout"])
    assert result.returncode == 0, result.stderr

    def calls(value):
        if isinstance(value, dict):
            if value.get("_type") == "SubroutineCallExpression":
                yield value["target"]
            for child in value.values():
                yield from calls(child)
        elif isinstance(value, list):
            for child in value:
                yield from calls(child)

    contract = next(root for root in roots if root["_type"] == "Contract")
    # Internal names use solc declaration IDs, not source spelling. Match the
    # declaration's source span, then require a call from the public read body.
    pointer_line = source[:source.index("function pointer(")].count("\n") + 2
    helpers = {method["member_name"] for method in contract["methods"]
               if method["source_location"]["line"] == pointer_line}
    read = next(method for method in contract["methods"] if method["member_name"] == "read")
    assert helpers
    assert any(target.get("member_name") in helpers for target in calls(read)), reason
