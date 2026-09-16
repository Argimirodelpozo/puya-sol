"""Independent pinned-solc expectations for every metamorphic source variant."""

import hashlib
import json
from pathlib import Path

import solcx
from Crypto.Hash import RIPEMD160
from eth_tester.exceptions import TransactionFailed
from web3 import EthereumTesterProvider, Web3
from web3.exceptions import ContractLogicError

# This host's OpenSSL omits RIPEMD-160. Supply the same standard primitive
# from PyCryptodome for PyEVM's precompile, without changing EVM execution.
try:
    hashlib.new("ripemd160")
except ValueError:
    original_hash = hashlib.new
    hashlib.new = lambda name, data=b"", **kwargs: (
        RIPEMD160.new(data) if name == "ripemd160" else original_hash(name, data, **kwargs))

repo = Path(__file__).resolve().parents[4]
template = (repo / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts/parenthesized_expressions.sol").read_text()
cases = [
    ("slotReferences", [False], [23, 33]),
    ("slotReferences", [True], [12, 33]),
    ("transientValue", [], [8, 0]),
    ("namedArrays", [], [7, 9]),
    ("constants", [], [bytes.fromhex("01020304"), bytes.fromhex("01020304"),
                       bytes.fromhex("9c1185a5c5e9fc54612808977ee8f548b2258d31")]),
    ("functionValues", [], [8, True]),
    ("effects", [], [41, 1]),
    ("customRequire", [True], 1),
    ("customRequire", [False], "REVERT"),
    ("tuplesAndArrays", [], [3, 3, 7]),
]
for via_ir in (False, True):
    for depth in (0, 1, 3):
        source = template.replace("/*(*/", "(" * depth).replace("/*)*/", ")" * depth)
        compiled = solcx.compile_standard({"language": "Solidity",
            "sources": {"Probe.sol": {"content": source}},
            "settings": {"evmVersion": "cancun", "viaIR": via_ir,
                "optimizer": {"enabled": True, "runs": 200},
                "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}},
            solc_binary=str(repo / "solidity/build/solc/solc"))
        artifact = compiled["contracts"]["Probe.sol"]["ParenthesizedExpressions"]
        web = Web3(EthereumTesterProvider())
        receipt = web.eth.wait_for_transaction_receipt(web.eth.send_transaction({
            "from": web.eth.accounts[0], "data": artifact["evm"]["bytecode"]["object"], "gas": 5000000}))
        assert receipt.status == 1
        contract = web.eth.contract(address=receipt.contractAddress, abi=artifact["abi"])
        for method, arguments, expected in cases:
            try:
                actual = getattr(contract.functions, method)(*arguments).call()
            except (TransactionFailed, ContractLogicError):
                actual = "REVERT"
            print(json.dumps({"via_ir": via_ir, "depth": depth, "method": method,
                "args": arguments, "expected": expected, "actual": actual,
                "match": actual == expected}, default=lambda value: value.hex()), flush=True)
            assert actual == expected, (via_ir, depth, method, expected, actual)
