"""PrivacyPool's fixed-array proof shape uses solc's memory-located members."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_modifier_fixed_array(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_fixed_array.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile]
        + (["--evm-storage-layout"] if slot else []))
    for seed in (0, 9, 2**255):
        if profile == "evm":
            selector = keccak.new(digest_bits=256, data=b"run(uint256)").digest()[:4]
            result = harness.call_raw(
                app, selector, extra_args=(encode(["uint256"], [seed]),),
                extra_fee=40_000, budget_pool=8)
            values = decode(["uint256[2]", "uint256[2][2]", "uint256[2]", "uint256"],
                            result.logs[-1][4:])
        else:
            values = harness.call(app, "run(uint256)", seed, extra_fee=40_000).abi_return
        assert [as_int(x) for x in values[0]] == [seed, 12]
        assert [[as_int(x) for x in row] for row in values[1]] == [[3, 44], [25, 6]]
        assert [as_int(x) for x in values[2]] == [7, 38]
        assert as_int(values[3]) == 25
