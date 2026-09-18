"""Raw array words use explicit solc-slot storage, never destructive ARC4 resizing."""
import pytest

from test_ast_audit import compile_app
from test_call_operands import invoke
from test_root_inventory import compile_source


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_raw_array_storage(harness, via_ir, profile):
    artifacts = compile_app(harness, "raw_array_storage", via_ir, profile, True)
    app = harness.deploy(artifacts, "RawArrayStorage", fund_wei=30_000_000)
    for method, expected in (("rootRoundtrip", (8,)), ("memberRoundtrip", (9, 7, 8)),
                             ("hiddenTail", (29, 29)), ("packedRoundtrip", (9,)),
                             ("nestedRoundtrip", (18,)), ("mappingRoundtrip", (19,)),
                             ("literalRoot", (8,))):
        assert invoke(harness, app, profile, method + "()", returns=["uint256"] * len(expected)) == expected
    assert invoke(harness, app, profile, "runtimeRoot(uint256)", [0]) == (8,)
    for whole in (False, True):
        assert invoke(harness, app, profile, "clearTail(bool)", [whole]) == (0,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_proven_scalar_slots_with_arrays(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "named_scalar_slots", via_ir, profile, slot)
    app = harness.deploy(artifacts, "NamedScalarSlots", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "scalarSlots()", returns=["uint256", "uint8", "bool", "uint256"]) == (7, 9, True, 33)
    assert invoke(harness, app, profile, "metadata()") == (2,)


REQUIRES_SLOTS = [
    "uint256[] a; function f() external { assembly { sstore(a.slot, 1) } }",
    "uint256[] a; function f() external view returns(uint256 r) { assembly { r := sload(a.slot) } }",
    "uint256[] a; function f() external { assembly { sstore(0, 1) } }",
    "uint256[] a; function f(uint256 p) external { assembly { sstore(p, 1) } }",
    "uint256[] a; function f(uint256 p) external view returns(uint256 r) { assembly { r := sload(p) } }",
    "uint8[2] a; function f() external { assembly { sstore(a.slot, 1) } }",
    "bytes a; function f() external { assembly { sstore(a.slot, 1) } }",
    "string a; function f() external { assembly { sstore(a.slot, 1) } }",
    "struct S { uint256 guard; uint256[] a; } S s; function f() external { "
    "uint256[] storage a = s.a; assembly { sstore(a.slot, 1) } }",
    "mapping(uint256 => uint256[]) m; function f(uint256 key) external { "
    "uint256[] storage a = m[key]; assembly { sstore(a.slot, 1) } }",
    "uint256[] a; function f() external { assembly { function write() { sstore(0, 1) } write() } }",
    "uint256[] a; function f(uint256 p) external { "
    "assembly { function write(x) { sstore(x, 1) } write(p) } }",
    "uint256[] a; function f() external { uint256 p; assembly { p := a.slot } "
    "assembly { sstore(p, 1) } }",
    "uint256[] a; function f() external { "
    "assembly { mstore(0, 0) sstore(keccak256(0, 32), 7) } }",
    "uint256[] a; modifier changes() { assembly { sstore(0, 1) } _; } function f() external changes {}",
    "function f(uint256 p) external returns(uint256) { uint256[] storage a; "
    "assembly { a.slot := p } return a.length; }",
    "uint256[] a; struct S { uint256 word; } function f(uint256 slot) external { "
    "S storage p; assembly { p.slot := slot } p.word = 5; }",
]


@pytest.mark.parametrize("body", REQUIRES_SLOTS)
@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_raw_array_access_requires_slot_mode(tmp_path, body, via_ir):
    source = "contract C { " + body + " }"
    flags = ["--via-yul-behavior"] if via_ir else []
    result, _, _ = compile_source(tmp_path, source, extra=flags)
    assert result.returncode != 0
    assert "--evm-storage-layout" in result.stderr and "array" in result.stderr
    assert "Internal compiler error" not in result.stderr
    result, _, _ = compile_source(tmp_path, source, extra=[*flags, "--evm-storage-layout"])
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("body", [
    "uint256[] a; function f() external { a.push(7); assembly { mstore(0, 3) } }",
    "uint256[] a; function f() external pure returns(uint256 r) { assembly { r := a.slot } }",
    "uint256[] a; function f() external returns(uint256 r) { "
    "assembly { function unused() { sstore(0, 1) } r := 7 } }",
    "struct S { uint256 guard; uint256[] a; } S s; function f() external view returns(uint256 r) { "
    "uint256[] storage a = s.a; assembly { r := a.slot } }",
])
def test_named_array_metadata_and_unrelated_assembly(tmp_path, body):
    result, _, _ = compile_source(tmp_path, "contract C { " + body + " }")
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("source", [
    "library L { function write() internal { assembly { sstore(0, 1) } } } "
    "contract C { uint256[] a; function f() external { L.write(); } }",
    "contract Base { uint256[] internal a; } contract C is Base { "
    "function f(uint256 p) external { assembly { sstore(p, 1) } } }",
    "contract C layout at 64 { uint256[] a; function f() external { assembly { sstore(64, 1) } } }",
    "contract C { struct Node { mapping(uint256 => Node) children; uint256[] values; } "
    "mapping(uint256 => Node) nodes; function f(uint256 p) external { assembly { sstore(p, 1) } } }",
])
def test_raw_array_host_layout_facts(tmp_path, source):
    result, _, _ = compile_source(tmp_path, source)
    assert result.returncode != 0
    assert "--evm-storage-layout" in result.stderr and "array" in result.stderr
    assert "Internal compiler error" not in result.stderr
    result, _, _ = compile_source(tmp_path, source, extra=["--evm-storage-layout"])
    assert result.returncode == 0, result.stderr
