"""Execute the typed-value regressions with official solc and an in-process EVM."""

import json
from pathlib import Path
import subprocess
import sys

from eth_tester.exceptions import TransactionFailed
from web3 import Web3, EthereumTesterProvider
from web3.exceptions import ContractLogicError

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts/eb_value_boundaries.sol"
SOLC = sys.argv[1]
rows = []
for via_ir in (False, True):
    request = {"language": "Solidity", "sources": {SOURCE.name: {"content": SOURCE.read_text()}},
               "settings": {"viaIR": via_ir, "optimizer": {"enabled": True}, "evmVersion": "cancun",
                            "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}}
    compiled = json.loads(subprocess.run([SOLC, "--standard-json"], input=json.dumps(request),
                                        text=True, capture_output=True, check=True).stdout)
    assert not [e for e in compiled.get("errors", []) if e["severity"] == "error"]
    chain = Web3(EthereumTesterProvider())
    sender = chain.eth.accounts[0]
    apps = {}
    for name, data in compiled["contracts"][SOURCE.name].items():
        factory = chain.eth.contract(abi=data["abi"], bytecode=data["evm"]["bytecode"]["object"])
        receipt = chain.eth.wait_for_transaction_receipt(factory.constructor().transact({"from": sender}))
        assert receipt.status == 1
        apps[name] = chain.eth.contract(address=receipt.contractAddress, abi=data["abi"])

    def check(signature, args, expected=None, *, reverts=False):
        try:
            actual = apps["EnumArrayPlaces"].get_function_by_signature(signature)(*args).call({"from": sender})
        except (TransactionFailed, ContractLogicError):
            assert reverts, (via_ir, signature, args)
            actual = "revert"
        else:
            assert not reverts and actual == expected, (via_ir, signature, args, actual, expected)
        rows.append({"via_ir": via_ir, "signature": signature, "args": args, "result": actual})

    for signature, args, expected in (
        ("memoryWrite()", [], 1), ("parameterWrite(uint8[2])", [[0, 2]], 12),
        ("dynamicWrite(uint8[])", [[0, 1]], 2), ("tupleWrite()", [], 12),
        ("nestedWrite()", [], 2), ("storageReference()", [], 1),
        ("deleteElement()", [], 1), ("storageWrite()", [], [1, 2]),
        ("evaluateOnce()", [], [1, 2]),
    ):
        check(signature, args, expected)
    check("dynamicWrite(uint8[])", [[]], reverts=True)
    for word in (0, 1, 2, 3, 2**64, 2**256 - 1):
        invalid = word >= 3
        for name in ("explicitReturn", "implicitReturn"):
            check(name + "(uint256)", [word], word, reverts=invalid)
        check("castDiscard(uint256)", [word], 7, reverts=invalid)
        for action in range(4):
            check("dirty(uint256,uint256)", [word, action],
                  (word, int(word != 0), word, 7)[action], reverts=invalid)

    app = apps["NamedArrayLengths"]
    for operation, expected in [(None, 0)] + [("push", n) for n in range(1, 10)] + [("pop", 8), ("clear", 0)]:
        if operation:
            receipt = chain.eth.wait_for_transaction_receipt(getattr(app.functions, operation)().transact({"from": sender}))
            assert receipt.status == 1
        actual = app.functions.lengths().call()
        assert actual == [expected] * 4
        rows.append({"via_ir": via_ir, "operation": operation, "lengths": actual})

    app = apps["ValueComparisons"]
    def mask(a, b):
        return sum(int(v) << i for i, v in enumerate((a == b, a != b, a < b, a <= b, a > b, a >= b)))
    for a, b in ((b"ab", b"ab\x00\x00"), (b"b\x00", b"aaaa"), (b"\x00\x00", b"\xff" * 4)):
        actual = app.functions.fixedBytes(a, b).call()
        assert actual == mask(a + bytes(2), b)
        rows.append({"via_ir": via_ir, "comparison": "bytes", "result": actual})
    for name, bits in (("narrow", 64), ("wide", 128)):
        for a, b in ((0, 0), (-1, 0), (0, -1), (-(2**(bits - 1)), 2**(bits - 1) - 1)):
            actual = getattr(app.functions, name)(a, b).call()
            assert actual == mask(a, b)
            rows.append({"via_ir": via_ir, "comparison": name, "args": [a, b], "result": actual})
    for n in (0, 1, 2**159 + 17, 2**160 - 1):
        for b in (False, True):
            raw = (2**160 - 1 - n).to_bytes(20, "big")
            actual = app.functions.converted(raw, n, b).call()
            assert actual == [raw, n.to_bytes(20, "big"), b]
            rows.append({"via_ir": via_ir, "conversion": n, "bool": b})

print(json.dumps({"solc": subprocess.check_output([SOLC, "--version"], text=True).strip(),
                  "checks": len(rows), "results": rows}, indent=2))
