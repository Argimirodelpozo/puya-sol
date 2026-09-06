"""Storage shape follows solc arrays/members even for non-ABI function types."""

import json

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int
from framework.compile import CompileError


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_dynamic_internal_function_storage(harness, slot_layout, abi, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_function_storage_shape.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    if not slot_layout:
        roots = json.loads((harness.out_dir / "awst.json").read_text())
        root = next(root for root in roots if root.get("name") == "FunctionStorageShape")
        kinds = {item["member_name"]: item["kind"] for item in root["app_state"]}
        for name in ("flat", "nested", "fixedDynamic", "dynamicStruct"):
            assert kinds[name] == 3  # Puya AppStorageKind.box
        for name in ("fixedStruct", "fixedArray"):
            assert kinds[name] == 1  # Puya AppStorageKind.app_global
    app = harness.deploy(artifacts, "FunctionStorageShape", exact_schema=True, fund_wei=10_000_000)

    def call(signature, args=(), returns=()):
        if abi == "arc4":
            result = harness.call(app, signature, *args, extra_fee=30_000)
            values = result.abi_return if returns else ()
        else:
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector,
                                      extra_args=(encode(["uint256"] * len(args), args),),
                                      extra_fee=30_000, budget_pool=14)
            values = decode(returns, result.logs[-1][4:]) if returns and not result.reverted else ()
        assert not result.reverted, result.fail_message
        return tuple(map(as_int, values))

    for _ in range(2):
        call("setup()")
        assert call("lengths()", returns=("uint256",) * 5 + ("uint16",) * 2) == (3, 1, 0, 1, 1, 7, 9)
        assert call("valuesA(uint256)", (5,), ("uint256",) * 4) == (10, 8, 10, 8)
        assert call("valuesB(uint256)", (5,), ("uint256",) * 4) == (10, 8, 8, 18)
        call("mutate()")
        assert call("changed(uint256)", (5,), ("uint256",) * 5) == (2, 8, 10, 10, 10)


@pytest.mark.parametrize("suffix", ["", "_field"])
def test_interior_dynamic_array_reference_is_diagnosed(harness, suffix):
    with pytest.raises(CompileError, match="dynamic-array storage references require a whole-box root"):
        harness.compile(f"puyasolRegression/contracts/rev_2_function_storage_interior{suffix}.sol")
