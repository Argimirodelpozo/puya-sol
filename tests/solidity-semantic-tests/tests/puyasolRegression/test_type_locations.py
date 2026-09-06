"""Value interning must not change Solidity's copy/reference semantics."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_value_type_locations(harness, slot_layout, abi):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_type_locations.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )
    signature = "run((uint16,uint16[2]))"
    value = (7, (8, 9))
    if abi == "arc4":
        result = harness.call(app, signature, value, extra_fee=20_000)
        assert not result.reverted, result.fail_message
        returned = result.abi_return
    else:
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(["(uint16,uint16[2])"], [value]),),
                                  extra_fee=20_000)
        assert not result.reverted, result.fail_message
        assert result.logs[-1].startswith(bytes.fromhex("151f7c75"))
        returned = decode(["uint16"] * 4, result.logs[-1][4:])
    assert tuple(map(as_int, returned)) == (7, 8, 8, 10)
