"""Independent expectations for reduction regressions, using the pinned solc."""

import json
from pathlib import Path

import solcx
from eth_abi import encode
from eth_tester.exceptions import TransactionFailed
from web3 import EthereumTesterProvider, Web3
from web3.exceptions import ContractLogicError

repo = Path(__file__).resolve().parents[4]
source = (repo / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts/storage_codec_yul_reductions.sol").read_text()
digest = lambda value: bytes(Web3.keccak(value))
memory_cases = [
    ("ints", [], encode(["int8[9]"], [[-1] + [0] * 7 + [127]])),
    ("bools", [], encode(["bool[9]"], [[True] + [False] * 7 + [True]])),
    ("fixedBytes", [], encode(["bytes4[5]"], [[bytes.fromhex("11223344")] + [bytes(4)] * 3 + [bytes.fromhex("aabbccdd")]])),
    ("nested", [], encode(["uint64[2][5]"], [[[17, 0]] + [[0, 0]] * 3 + [[0, 99]]])),
    ("dynamicElements", [], encode(["bytes[5]"], [[bytes.fromhex("aa22"), b"", b"", b"", bytes.fromhex("334455")]])),
    ("largeFixed", [], digest(encode(["uint256[65]"], [[11] + [0] * 63 + [99]]))),
    ("fullFixed", [], digest(encode(["uint256[128]"], [[11] + [0] * 126 + [99]]))),
] + [("bytesCopy", [n], [n, 97 if n else 0, 0]) for n in (0, 1, 31, 32, 33, 4065, 4095, 4096)]
yul_cases = [("boolSwitch", [x], 7) for x in (0, 2, 3, 8)] + [
    ("stringSwitch", [x], 11 if x == 97 else 22) for x in (0, 97, 98)] + [
    ("overlap", [], digest(bytes(32))),
    ("calldataCoincidence", [(11, 22)], digest(bytes(64))),
    ("hugeHash", [], "REVERT"),
    ("hashRange", [2**256 - 1, 0], digest(b"")),
    ("poisonedAlignment", [], 1),
    ("previousBlockAlignment", [], 1),
] + [("dynamicCoincidence", [b"x" * n], digest(bytes(n + 32))) for n in (0, 1, 16, 32)] + [
    ("results", [x], [2*x+4, 2*x+5, 2]) for x in (0, 17)] + [
    ("revertRange", [n], "REVERT") for n in (0, 1, 31, 32)]
storage_cases = [("packed", list(values), True) for values in (
    (-128, -(1 << 39), -(1 << 127)), (-1, -1, -1), (0, 0, 0),
    (127, (1 << 39)-1, (1 << 127)-1))]

for via_ir in (False, True):
    compiled = solcx.compile_standard({"language": "Solidity",
        "sources": {"Probe.sol": {"content": source}},
        "settings": {"evmVersion": "cancun", "viaIR": via_ir,
            "optimizer": {"enabled": True, "runs": 200},
            "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}},
        solc_binary=str(repo / "solidity/build/solc/solc"))
    web = Web3(EthereumTesterProvider())
    for name, cases in (("StorageCodecReductions", storage_cases),
                        ("MemoryCodecReductions", memory_cases), ("YulReductions", yul_cases)):
        artifact = compiled["contracts"]["Probe.sol"][name]
        receipt = web.eth.wait_for_transaction_receipt(web.eth.send_transaction({
            "from": web.eth.accounts[0], "data": artifact["evm"]["bytecode"]["object"], "gas": 5000000}))
        assert receipt.status == 1
        contract = web.eth.contract(address=receipt.contractAddress, abi=artifact["abi"])
        for method, args, expected in cases:
            try:
                actual = getattr(contract.functions, method)(*args).call({"gas": 1000000})
            except (TransactionFailed, ContractLogicError):
                actual = "REVERT"
            print(json.dumps({"via_ir": via_ir, "contract": name, "method": method,
                "args": args, "expected": expected, "actual": actual, "match": actual == expected},
                default=lambda value: value.hex()), flush=True)
            assert actual == expected, (via_ir, name, method, expected, actual)
