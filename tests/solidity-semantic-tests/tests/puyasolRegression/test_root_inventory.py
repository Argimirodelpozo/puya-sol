"""Root inventory, actual LogicSig marker identity, and imported source spans."""

import json
import subprocess

import pytest
from framework.paths import COMPILER, PUYA


def compile_source(tmp_path, source, *, backend=False, extra=()):
    path = tmp_path / "Main.sol"
    path.write_text("pragma solidity ^0.8.20;\n" + source)
    output = tmp_path / "output"
    result = subprocess.run([str(COMPILER), "--source", str(path), "--import-path", str(tmp_path),
                             "--output-dir", str(output), "--puya-path", str(PUYA),
                             *([] if backend else ["--no-puya"]), *extra],
                            text=True, capture_output=True, timeout=180)
    roots = json.loads((output / "awst.json").read_text()) if (output / "awst.json").exists() else []
    return result, roots, output


def test_logicsig_name_is_not_identity(tmp_path):
    result, roots, _ = compile_source(tmp_path, """
        contract LogicSig { modifier logicsig() { _; }
            function approve() public pure logicsig returns(bool) { return true; }
        }
    """, backend=True)
    assert result.returncode == 0, result.stderr
    assert any(root["_type"] == "Contract" and root["name"] == "LogicSig" for root in roots)
    assert not any(root["_type"] == "LogicSignature" for root in roots)


@pytest.mark.parametrize("body", [
    "function a() public pure logicsig returns(bool) { return true; } "
    "function b() public pure logicsig returns(bool) { return false; }",
    "function a(uint64 x) public pure logicsig returns(bool) { return x > 0; }",
    "function a() public pure logicsig returns(uint64) { return 1; }",
    "function a() public pure returns(bool) { return true; } "
    "function b() public pure returns(bool) { return false; }",
])
def test_logicsig_rejects_ambiguous_or_invalid_entry(tmp_path, body):
    result, _, _ = compile_source(tmp_path,
        'import {LogicSig} from "libs/AVM.sol"; contract Signature is LogicSig {' + body + '}')
    assert result.returncode != 0
    assert "entry" in result.stderr and "LogicSig" in result.stderr


@pytest.mark.parametrize("inherited", [False, True])
def test_logicsig_helpers_and_resolved_entry(tmp_path, inherited):
    base = "abstract contract Base" if inherited else "contract Signature"
    source = 'import {LogicSig} from "libs/AVM.sol"; ' + base + """ is LogicSig {
        function approve() public pure logicsig returns(bool) { return helper(); }
        function approve(uint64 x) public pure returns(bool) { return x == 7; }
        function helper() internal pure returns(bool) { return true; }
    }""" + ("contract Signature is Base {}" if inherited else "")
    result, roots, _ = compile_source(tmp_path, source, backend=True)
    assert result.returncode == 0, result.stdout + result.stderr
    signatures = [root for root in roots if root["_type"] == "LogicSignature"]
    assert len(signatures) == 1
    assert signatures[0]["program"]["name"] == "approve()"
    assert any(root["_type"] == "Subroutine" for root in roots)


@pytest.mark.parametrize("mixed", [False, True])
@pytest.mark.parametrize("reverse", [False, True])
def test_all_deployable_libraries(tmp_path, mixed, reverse):
    libraries = [f"library {name} {{ function value() external pure returns(uint64) {{ return {n}; }} }}"
                 for n, name in enumerate(("FirstLibrary", "SecondLibrary"), 1)]
    if reverse:
        libraries.reverse()
    source = "\n".join(libraries)
    if mixed:
        source += "contract Ordinary { function value() external pure returns(uint64) { return 3; } }"
    result, roots, output = compile_source(tmp_path, source, backend=True)
    assert result.returncode == 0, result.stdout + result.stderr
    names = {root["name"] for root in roots if root["_type"] == "Contract"}
    assert names == {"FirstLibrary", "SecondLibrary"} | ({"Ordinary"} if mixed else set())
    for name in names:
        assert (output / (name + ".approval.bin")).is_file(), name


def test_imported_source_location(tmp_path):
    library = tmp_path / "Library.sol"
    library.write_text("pragma solidity ^0.8.20;\n\nlibrary Library {\n"
                       "    function add(uint64 n) internal pure returns(uint64) { return n + 1; }\n}\n")
    result, roots, _ = compile_source(tmp_path, """
        import {Library} from "Library.sol";
        contract C { function f(uint64 n) public pure returns(uint64) { return Library.add(n); } }
    """)
    assert result.returncode == 0, result.stderr
    sub = next(root for root in roots if root["_type"] == "Subroutine" and root["name"] == "Library.add")
    span = sub["source_location"]
    assert span["file"] == str(library)
    assert span["line"] == span["end_line"] == 4
    assert span["column"] == 4 and span["end_column"] > 4

    def nodes(value):
        if isinstance(value, dict):
            if "_type" in value:
                yield value
            for child in value.values():
                yield from nodes(child)
        elif isinstance(value, list):
            for child in value:
                yield from nodes(child)

    expressions = [node for node in nodes(sub) if node["_type"] in
                   {"UInt64BinaryOperation", "IntegerConstant", "VarExpression"}]
    assert expressions
    for expression in expressions:
        assert expression["source_location"]["file"] == str(library), expression
        assert expression["source_location"]["line"] == 4, expression


def test_logicsig_helper_runtime(tmp_path, localnet):
    from algosdk.error import AlgodHTTPError
    from algosdk.transaction import LogicSigAccount, LogicSigTransaction, PaymentTxn, wait_for_confirmation

    result, _, output = compile_source(tmp_path, '''
        import {LogicSig, Txn} from "libs/AVM.sol";
        contract Signature is LogicSig {
            function approve() public view logicsig returns(bool) { return helper(); }
            function helper() internal view returns(bool) { return Txn.fee() == 1000; }
        }
    ''', backend=True)
    assert result.returncode == 0, result.stdout + result.stderr
    program = next(output.glob("Signature*.bin")).read_bytes()
    signature = LogicSigAccount(program)
    signature.sign(localnet.account.private_key)

    def payment(fee):
        params = localnet.algod.suggested_params()
        params.flat_fee, params.fee = True, fee
        return LogicSigTransaction(PaymentTxn(localnet.account.address, params,
            localnet.account.address, 0, note=str(tmp_path).encode()), signature)

    txid = localnet.algod.send_transaction(payment(1000))
    assert wait_for_confirmation(localnet.algod, txid, 4)["confirmed-round"] > 0
    with pytest.raises(AlgodHTTPError, match="rejected by logic"):
        localnet.algod.send_transaction(payment(2000))
