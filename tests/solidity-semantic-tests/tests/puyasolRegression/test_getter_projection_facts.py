"""The generated interface selects fields; the physical backend only reads them."""

import pytest
from framework import as_signed_int
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_getter_projection_facts(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy("puyasolRegression/contracts/getter_projection_facts.sol",
                                    via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", profile]
                                    + (["--evm-storage-layout"] if slot else []))
    types = ["int8", "uint128", "bool", "bytes5", "bytes"]
    assert invoke(harness, app, profile, "keyed(uint64)", [0], types) == (0, 0, False, bytes(5), b"")
    invoke(harness, app, profile, "rows(uint256)", [0], reverts=True)
    invoke(harness, app, profile, "ranked(uint64,uint256)", [4, 2], reverts=True)
    # Void calls are deliberately not decoded by invoke's value-only helper.
    if profile == "evm":
        from Crypto.Hash import keccak
        result = harness.call_raw(app, keccak.new(digest_bits=256, data=b"populate()").digest()[:4],
                         extra_args=(b"",), extra_fee=40_000, budget_pool=8)
    else:
        result = harness.call(app, "populate()", extra_fee=40_000)
    assert not result.reverted, result.fail_message
    expected = (-7, 2**100 + 3, True, bytes.fromhex("0102030405"), bytes.fromhex("123456"))
    for signature, arguments in (("keyed(uint64)", [3]), ("ranked(uint64,uint256)", [4, 1]),
                                  ("rows(uint256)", [0])):
        values = invoke(harness, app, profile, signature, arguments, types)
        assert (as_signed_int(values[0]), *values[1:]) == expected
    assert as_signed_int(invoke(harness, app, profile, "single()", returns=["int72"])[0]) == -9
