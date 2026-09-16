"""Independent pinned-solc expectations for the outstanding audit fixes."""

import json
from pathlib import Path

import solcx
from eth_abi import encode
from eth_tester.exceptions import TransactionFailed
from web3 import EthereumTesterProvider, Web3
from web3.exceptions import ContractLogicError

repo = Path(__file__).resolve().parents[4]
fixtures = repo / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts"
sources = {name + ".sol": {"content": (fixtures / (name + ".sol")).read_text()}
           for name in ("storage_codec_correctness", "memory_layout_correctness", "rev_2_array_conversion_loop")}
lengths = [(method, [n], "REVERT" if n > 1 else 1 if method.startswith("replace") else n)
           for method in ("copyFlags", "copyWords", "copyNested", "replaceFlags", "replaceWords")
           for n in (0, 1, 2**64, 2**64 + 1, 2**128 + 1, 2**256 - 1)]
enum_words = (0, 1, 2, 7, 255, 257, 2**64 + 1, 2**256 - 1)
enums = [("encodeEnum", [n, packed], "REVERT" if n >= 2 else n.to_bytes(1 if packed else 32, "big"))
         for n in enum_words for packed in (False, True)] + [
         (method, [n], "REVERT" if cleaned >= 2 else cleaned)
         for method in ("storedEnum", "explicitEnum") for n in enum_words
         for cleaned in [n & 255 if method == "storedEnum" else n]] + [
         ("rawEnum", [n], n) for n in enum_words]
enums += [(method, args, "REVERT" if n >= 2 else n) for n in enum_words
          for method, args in (("namedEnum", [n]), ("callEnum", [n, False]), ("callEnum", [n, True]))]
memory = [("nested", [], encode(["uint64[2][5]"], [[[17, 0]] + [[0, 0]] * 3 + [[0, 99]]])),
          ("byteAssignments", [], encode(["bytes[5]"], [[bytes.fromhex("aa22"), b"", b"", b"", bytes.fromhex("334455")]])),
          ("zeroPointer", [], [96, 96, 96])] + [(method, [], True) for method in (
              "distinct", "structDefault", "freshZero", "aliases", "tupleAliases", "deletedReferences",
              "highLevelAliases", "freshFunctionArrays")]
narrow = [(method, [n], [n, clean]) for n in (0, 1, 257, 2**64 + 1, 2**256 - 1)
          for method, clean in (("uintWord", n & 255), ("boolWord", bool(n)),
                                ("bytesWord", (n >> 248).to_bytes(1, "big")))]
cases = {"ArrayConversionLoop": [("run", [], [2, -3, 0, -3, 0])],
         "StorageLengthChecks": lengths,
         "StorageTraversalChecks": [("roundTrip", [], True), ("clearStruct", [], True), ("clearPacked", [], True)],
         "PackedAddressChecks": [(method, [], True) for method in ("clearRaw", "keepNeighbor", "changeRestore")]
            + [("mapped", list(args), True) for args in ((False, False), (True, False), (False, True))],
         "EnumBoundaryChecks": enums, "MemoryLayoutChecks": memory, "NarrowYulChecks": narrow}

failures = []
for via_ir in (False, True):
    compiled = solcx.compile_standard({"language": "Solidity", "sources": sources,
        "settings": {"evmVersion": "cancun", "viaIR": via_ir,
            "optimizer": {"enabled": True, "runs": 200},
            "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}}}},
        solc_binary=str(repo / "solidity/build/solc/solc"))
    web = Web3(EthereumTesterProvider())
    for artifacts in compiled["contracts"].values():
        for name, artifact in artifacts.items():
            receipt = web.eth.wait_for_transaction_receipt(web.eth.send_transaction({
                "from": web.eth.accounts[0], "data": artifact["evm"]["bytecode"]["object"], "gas": 8000000}))
            assert receipt.status == 1
            contract = web.eth.contract(address=receipt.contractAddress, abi=artifact["abi"])
            for method, args, expected in cases[name]:
                # Legacy solc wraps this forged maximal array extent and returns
                # its length without traversing. AVM explicitly rejects values
                # outside its materialization capacity; via-IR rejects it too.
                if not via_ir and name == "StorageLengthChecks" and method in ("copyFlags", "copyWords") and args == [2**256 - 1]:
                    expected = 2**256 - 1
                try:
                    actual = getattr(contract.functions, method)(*args).call({"gas": 6000000})
                except (TransactionFailed, ContractLogicError):
                    actual = "REVERT"
                print(json.dumps({"via_ir": via_ir, "contract": name, "method": method,
                    "args": args, "expected": expected, "actual": actual, "match": actual == expected},
                    default=lambda value: value.hex()), flush=True)
                if actual != expected:
                    failures.append((via_ir, name, method, args, expected, actual))
assert not failures, failures
