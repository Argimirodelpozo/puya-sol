"""Missing explorer arguments are recoverable only with an exact solc proof."""
import copy
import json
import subprocess

import pytest

import fetch
from chd_common import EVM_PY, HERE


@pytest.fixture(scope="module")
def verified_constructor():
    script = r'''
import json
import solcx
from eth_abi import encode
source = """pragma solidity ^0.8.26;
contract C {
    struct Item { address who; uint256[] values; bytes raw; }
    bytes32 public digest;
    constructor(Item[] memory items, string memory label) {
        digest = keccak256(abi.encode(items, label));
    }
}
"""
settings = {"optimizer": {"enabled": True, "runs": 123}, "evmVersion": "paris"}
result = solcx.compile_standard({"language": "Solidity", "sources": {
    "verified/C.sol": {"content": source}}, "settings": settings | {
    "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}},
    solc_version="0.8.26")["contracts"]["verified/C.sol"]["C"]
args = encode(["(address,uint256[],bytes)[]", "string"], [
    [("0x" + "12" * 20, [1, 2**255], b"abc")], "proof"]).hex()
print(json.dumps({"name": "C", "file_path": "verified/C.sol", "source_code": source,
    "compiler_version": "v0.8.26+commit.8a97fa7a", "compiler_settings": settings,
    "abi": result["abi"], "constructor_args": None, "expected_args": args,
    "creation_bytecode": "0x" + result["evm"]["bytecode"]["object"] + args}))
'''
    result = subprocess.run([str(EVM_PY), "-c", script], capture_output=True,
                            text=True, timeout=60, check=True)
    return json.loads(result.stdout)


def recover(sc):
    return subprocess.run([str(EVM_PY), str(HERE / "chd_constructor.py")],
                          input=json.dumps(sc), capture_output=True, text=True, timeout=60)


def test_exact_solc_recovery_handles_nested_dynamic_constructor_values(verified_constructor):
    facts = fetch.constructor_facts(verified_constructor)
    assert facts["ctor_args_hex"] == verified_constructor["expected_args"]
    proof = facts["ctor_args_recovery"]
    assert proof["solc_version"] == "0.8.26"
    assert proof["argument_bytes"] == len(facts["ctor_args_hex"]) // 2
    assert proof["settings"]["optimizer"] == {"enabled": True, "runs": 123}
    assert set(proof["source_sha256"]) == {"verified/C.sol"}


@pytest.mark.parametrize("change", ["bytecode", "source", "settings", "abi", "trailing", "truncated"])
def test_recovery_rejects_unproven_or_noncanonical_suffixes(verified_constructor, change):
    sc = copy.deepcopy(verified_constructor)
    if change == "bytecode":
        sc["creation_bytecode"] = "0x00" + sc["creation_bytecode"][4:]
    elif change == "source":
        sc["source_code"] += "\n// Different source metadata\n"
    elif change == "settings":
        sc["compiler_settings"]["optimizer"]["enabled"] = False
    elif change == "abi":
        next(e for e in sc["abi"] if e["type"] == "constructor")["inputs"] = []
    elif change == "trailing":
        sc["creation_bytecode"] += "00" * 32
    else:
        sc["creation_bytecode"] = sc["creation_bytecode"][:-64]
    assert recover(sc).returncode != 0


def test_supplied_arguments_and_missing_evidence_do_not_invoke_compiler(monkeypatch):
    def unexpected(*args, **kwargs):
        raise AssertionError("unexpected recovery compiler")
    monkeypatch.setattr(subprocess, "run", unexpected)
    abi = [{"type": "constructor", "inputs": [{"type": "uint256"}]}]
    assert fetch.constructor_facts({"abi": abi, "constructor_args": "0x1234"}) == {
        "ctor_args_hex": "1234"}
    assert fetch.constructor_facts({"abi": abi}) == {"ctor_args_hex": ""}
    assert fetch.constructor_facts({"abi": [], "creation_bytecode": "0x1234"}) == {
        "ctor_args_hex": ""}


def test_importer_surfaces_failed_recovery(verified_constructor):
    sc = verified_constructor | {"creation_bytecode": "0x1234"}
    with pytest.raises(ValueError, match="exact deployment prefix"):
        fetch.constructor_facts(sc)


def test_dependency_fetch_retains_recovery_evidence(tmp_path, monkeypatch, verified_constructor):
    monkeypatch.setattr(fetch, "http_json", lambda _url: verified_constructor)
    result = fetch.fetch_dep("example.test", "0x" + "34" * 20, tmp_path, 1, set())
    assert result["ctor_args_hex"] == verified_constructor["expected_args"]
    assert result["ctor_args_recovery"]["method"].startswith("exact verified solc")
    assert json.loads((tmp_path / "case.json").read_text()) == result
