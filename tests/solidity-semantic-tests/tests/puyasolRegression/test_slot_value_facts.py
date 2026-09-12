"""Declared-type slot codecs, aggregate cleanup and full AVM account values."""

import pytest
from Crypto.Hash import keccak
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_slot_value_facts(harness, via_ir, profile):
    app = harness.compile_and_deploy("puyasolRegression/contracts/slot_value_facts.sol",
                                    via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", profile, "--evm-storage-layout"],
                                    fund_wei=40_000_000)
    first = bytes.fromhex("aa" * 12 + "11" * 20)
    second = bytes.fromhex("bb" * 12 + "22" * 20)
    assert invoke(harness, app, profile, "fixedAccounts(address,address)", [first, second], ["bool"] * 2) == (True, True)
    assert invoke(harness, app, profile, "packedAccount(address)", [first], ["bool"] * 3) == (True, True, True)
    assert invoke(harness, app, profile, "standaloneAccount(address)", [second], ["bool"] * 2) == (True, True)
    assert invoke(harness, app, profile, "fixedStrings()", returns=["uint64"] * 4) == (1, 40, 0, 0)
    hash_ = lambda value: keccak.new(digest_bits=256, data=bytes.fromhex(value)).digest()
    assert invoke(harness, app, profile, "fixedBytes()", returns=["bytes32", "bytes32", "uint64", "uint64"]) == (
        hash_("001122"), hash_("0123456789012345678901234567890123456789012345678901234567890123456789"), 0, 0)
    assert invoke(harness, app, profile, "shorterNested()", returns=["uint256"] * 3) == (11, 22, 0)
    for n in (0, 1, 9, 33):
        assert invoke(harness, app, profile, "booleans(uint256)", [n], ["uint256"] * 3) == ((n + 2) // 3, 0, 0), n
