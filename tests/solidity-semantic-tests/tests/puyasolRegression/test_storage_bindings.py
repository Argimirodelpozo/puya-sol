"""Per-contract binding facts must not leak across inherited declarations."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int


@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_storage_bindings_are_contract_scoped(harness, slot_layout, abi):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_storage_bindings.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
    )

    def call(app, signature, args=(), returns=()):
        if abi == "arc4":
            result = harness.call(app, signature, *args, extra_fee=20_000)
            values = result.abi_return if returns else ()
        else:
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector,
                                      extra_args=(encode(["uint16"] * len(args), args),),
                                      extra_fee=20_000)
            values = decode(returns, result.logs[-1][4:]) if returns and not result.reverted else ()
        assert not result.reverted, result.fail_message
        return tuple(map(as_int, values))

    for name in ("BindingLR", "BindingRL", "BindingLeft"):
        app = harness.deploy(artifacts, name, exact_schema=True)
        pair = ("uint16", "uint256")
        assert call(app, "left()", returns=pair) == (7, 0)
        call(app, "setLeft(uint16)", (11,))
        if name != "BindingLeft":
            assert call(app, "right()", returns=pair) == (9, 0)
            call(app, "setRight(uint16)", (22,))
            assert call(app, "right()", returns=pair) == (22, 1)
        assert call(app, "left()", returns=pair) == (11, 1)
        call(app, "clearLeft()")
        assert call(app, "left()", returns=pair) == (11, 0)
        call(app, "setLeft(uint16)", (33,))
        assert call(app, "left()", returns=pair) == (33, 1)
        if name != "BindingLeft":
            assert call(app, "right()", returns=pair) == (22, 1)
