"""Reproduce the boundary oracle with official solc, web3 and eth-tester[py-evm].

Usage: python solc_oracle.py /path/to/solc-v0.8.34 > solc-oracle.json
The frontend uses its separately pinned vendored solc; this is an EVM oracle.
"""

import json
from pathlib import Path
import subprocess
import sys

from eth_abi import decode
from eth_tester.exceptions import TransactionFailed
from web3 import Web3, EthereumTesterProvider

ROOT = Path(__file__).resolve().parents[4]
SOLC = sys.argv[1]
FIXTURES = ROOT / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts"
CASES = {
    "RawScalars": [(name + "()", []) for name in (
        "u8", "u64", "u128", "boolean", "fixedByte", "signedByte", "sameBlock",
        "highLevelRead", "highLevelWrite", "tupleWrites", "increment", "clear")]
        + [("branch(bool)", [x]) for x in (False, True)]
        + [("loop(uint256)", [x]) for x in (0, 1, 2)]
        + [("parameter(uint8)", [7])]
        + [("copies(bool)", [x]) for x in (False, True)]
        + [("namedReturn(uint256)", [x]) for x in (0, 257, 2**64 + 1, 2**256 - 1)],
    "Alias": [("f(bytes)", [b"abc"])],
    "SliceAlias": [("f(bytes)", [b"abc"])],
    "Rebind": [("f(bytes,bytes)", [b"a", b"bc"])],
    "ConditionalSeed": [("f(bytes,bool)", [b"abc", x]) for x in (False, True)],
    "LoopSeed": [("f(bytes,uint256)", [b"abc", x]) for x in (0, 1, 2)],
    "PointerControl": [("f(bytes,bytes)", [b"a", b"bc"])],
    "CalldataSnapshot": [(name + "(uint256)", [7]) for name in
                         ("fixedOffset", "acrossBlocks", "sameBlock", "control")]
                         + [("dynamicOffset(uint256,uint256)", [7, 4]), ("messageAlias(uint256)", [7])],
    "StaticAlias": [("f(uint256[2])", [[11, 22]])],
    "ArrayAlias": [("f(uint256[])", [[11, 22]])],
    "DynamicFixedPointer": [("f(bytes[2])", [[b"a", b"bc"]])],
    "DynamicStructPointer": [("f((bytes))", [[b"abc"]])],
    "LiveArrayLength": [("f(uint256[])", [[11, 22]])],
    "LiveArrayElement": [("f(uint256[])", [[11, 22]])],
    "ReferenceVariants": [(name + "(bytes,bytes)", [b"a", b"bc"]) for name in ("swap", "declare")]
        + [("select(bytes,bytes,bool)", [b"a", b"bc", x]) for x in (False, True)]
        + [("typed(uint256[],uint256[])", [[11, 22], [33]])],
    "InternalCalldataFrame": [("run(uint256,bytes)", [7, b"abc"]),
                              ("throughPublic(uint256,bytes)", [7, b"abc"])]
        + [(name + "(uint256,bytes,bool)", [7, b"abc", x])
           for name in ("indirect", "mixedPointer") for x in (False, True)],
    "VirtualCalldataFrame": [("run(uint256)", [7])],
    "ModifierCalldataFrame": [("run(bytes)", [b"abc"])],
}


def normalize(value):
    if isinstance(value, (bytes, bytearray)):
        return {"hex": value.hex()}
    if isinstance(value, (list, tuple)):
        return [normalize(item) for item in value]
    return value


def deploy(chain, filename, via_ir):
    request = {"language": "Solidity", "sources": {filename: {"content": (FIXTURES / filename).read_text()}},
               "settings": {"evmVersion": "cancun", "viaIR": via_ir, "optimizer": {"enabled": True},
                            "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}}
    result = subprocess.run([SOLC, "--standard-json"], input=json.dumps(request), text=True,
                            capture_output=True, check=True, timeout=120)
    compiled = json.loads(result.stdout)
    errors = [e["formattedMessage"] for e in compiled.get("errors", []) if e["severity"] == "error"]
    assert not errors, errors
    apps = {}
    for name, data in compiled["contracts"][filename].items():
        factory = chain.eth.contract(abi=data["abi"], bytecode=data["evm"]["bytecode"]["object"])
        receipt = chain.eth.wait_for_transaction_receipt(factory.constructor().transact({"from": chain.eth.accounts[0]}))
        apps[name] = chain.eth.contract(address=receipt.contractAddress, abi=data["abi"])
    return apps


runs = []
for via_ir in (False, True):
    chain = Web3(EthereumTesterProvider())
    sender = chain.eth.accounts[0]
    apps = deploy(chain, "assembly_boundaries.sol", via_ir)
    rows = []
    for name, calls in CASES.items():
        for signature, arguments in calls:
            result = apps[name].get_function_by_signature(signature)(*arguments).call({"from": sender})
            rows.append({"contract": name, "signature": signature, "args": arguments, "result": result})
    result = decode(["uint256"] * 4, chain.eth.call(
        {"from": sender, "to": apps["FallbackCalldataFrame"].address, "data": "0x123456780102"}))
    assert result == (6, 0, 6, 0x12)
    rows.append({"contract": "FallbackCalldataFrame", "result": result})
    modified = apps["ModifierCalldataFrame"]
    receipt = chain.eth.wait_for_transaction_receipt(modified.functions.noReturn(b"abc").transact({"from": sender}))
    assert receipt.status == 1
    result = modified.functions.observed().call()
    assert result == 97 * 256 + 254
    rows.append({"contract": "ModifierCalldataFrame", "signature": "noReturn(bytes)", "observed": result})

    apps = deploy(chain, "lowering_empty_calls.sol", via_ir)
    app = apps["LoweringEmptyCalls"]
    result = app.functions.zero().call()
    assert result == [32, 0, 0]
    rows.append({"case": "zero", "result": result})
    for name, expected in (("EmptyCallReceiver", b""), ("EmptyCallFallback", bytes.fromhex("123456"))):
        for literal in (False, True):
            result = app.functions.run(apps[name].address, 0, b"", literal).call()
            assert result == [expected, len(expected)], result
            rows.append({"case": name, "literal": literal, "result": result})
    for literal in (False, True):
        for target in (chain.eth.accounts[1], "0x0000000000000000000000000000000000000000"):
            result = app.functions.noValue(target, b"", literal).call()
            assert result == [b"", 0], result
            rows.append({"case": "no-value", "target": target, "literal": literal, "result": result})
        result = app.functions.run(chain.eth.accounts[1], 0, b"", literal).call()
        assert result == [b"", 0], result
        rows.append({"case": "account", "literal": literal, "result": result})
        try:
            app.functions.run(apps["EmptyCallRejecting"].address, 0, b"", literal).call()
        except TransactionFailed:
            rows.append({"case": "rejecting", "literal": literal, "reverts": True})
        else:
            raise AssertionError("Rejecting receiver succeeded")
    runs.append({"via_ir": via_ir, "rows": normalize(rows)})

assert runs[0]["rows"] == runs[1]["rows"], "Legacy/via-IR disagreement"
print(json.dumps({"solc": subprocess.check_output([SOLC, "--version"], text=True),
                  "executions": sum(len(run["rows"]) for run in runs), "runs": runs},
                 default=normalize, indent=2))
