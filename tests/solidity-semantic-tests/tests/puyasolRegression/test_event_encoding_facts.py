"""ARC-28 event identity follows the emitted type, EVM selectors follow solc."""

import hashlib

import pytest
from algosdk.abi import ABIType
from Crypto.Hash import keccak
from eth_abi import decode


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile,evm_selectors", [("arc4", False), ("arc4", True), ("evm", True)])
def test_event_encoding_facts(harness, via_ir, profile, evm_selectors):
    artifacts = harness.compile("puyasolRegression/contracts/event_encoding_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-selectors"] if evm_selectors else []))
    app = harness.deploy(artifacts, "EventEncodingFacts", fund_wei=20_000_000)
    for method, name, wire, canonical, values in [
        ("value", "Value", "((uint8,uint16))", "((uint8,uint16))", [[7, 300]]),
        ("array", "Array", "((uint8,uint16)[])", "((uint8,uint16)[])", [[[7, 300], [8, 400]]]),
        ("mixed", "Mixed", "(uint64,uint256,bool,byte[],string)", "(uint8,uint128,bool,bytes,string)",
         [7, 9, True, b"\x01\x02", "text"]),
        ("widen", "Widen", "(uint256)", "(uint256)", [7]),
        ("empty", "Empty", "()", "()", []),
        ("aliasMutated", "Seen", "((uint8,uint16),uint64)", "((uint8,uint16),uint64)", [[9, 300], 11]),
    ]:
        if profile == "evm":
            selector = keccak.new(digest_bits=256, data=(method + "()").encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(b"",), extra_fee=40_000, budget_pool=8)
            returned = decode(["bytes32"], result.logs[-1][4:])[0]
        else:
            result = harness.call(app, method + "()", extra_fee=40_000)
            returned = bytes(result.abi_return)
        digest = hashlib.new("sha512_256", (name + wire).encode()).digest()
        expected = keccak.new(digest_bits=256, data=(name + canonical).encode()).digest() if evm_selectors else digest
        assert returned == expected, method
        payload = ABIType.from_string(wire).encode(values) if values else b""
        assert result.logs[0] == digest[:4] + payload, method
