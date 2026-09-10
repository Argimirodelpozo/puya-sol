"""Modifier input/output lifetimes, checked against solc 0.8.34 in both modes."""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_modifier_returns(harness, via_ir, slot_layout, profile):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_returns.sol", contract_name="ModifierReturns",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))

    def check(signature, arguments, expected, returns=("uint64",)):
        if profile == "arc4":
            result = harness.call(app, signature, *arguments)
            assert not result.reverted, result.fail_message
            values = (result.abi_return,) if len(returns) == 1 else result.abi_return
            actual = tuple((as_signed_int if sol_type.startswith("int") else as_int)(value)
                           for sol_type, value in zip(returns, values, strict=True))
        else:
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),))
            assert not result.reverted, result.fail_message
            actual = decode(returns, result.logs[-1][4:])
        assert actual == expected, (signature, arguments)

    for x in (0, 7, 2**32):
        for signature in ("f(uint64)", "explicitReturn(uint64)"):
            check(signature, [x], (x if via_ir else 2 * x,))
        check("nested(uint64)", [x], (x if via_ir else 4 * x,))
        for n in (0, 1, 3):
            check("looped(uint64,uint64)", [x, n], (x * (bool(n) if via_ir else n),))
        check("signedTuple(uint64)", [x], (x, -2) if via_ir else (2 * x, -4), ("uint64", "int16"))
        check("allUnnamed(uint64)", [x], (x, -3), ("uint64", "int16"))
        check("seeded(uint64)", [x], (5 + (x if via_ir else 2 * x),))
        check("perInvocation(uint64)", [x], (3 + x if via_ir else 6 + 2 * x,))
        for run in (False, True):
            check("skipped(uint64,bool)", [x, run], (5 + x if run else (0 if via_ir else 5),))
            check("nestedSkip(uint64,bool)", [x, run], (9 + x if run else (5 if via_ir else 9),))
        for before in (False, True):
            for after in (False, True):
                check("earlyReturn(uint64,bool,bool)", [x, before, after],
                      (0 if before else (x if via_ir or after else 2 * x),))
        # Reusing return inputs must not undo actual memory/storage mutations.
        check("memoryWriteBack(uint64)", [x], (x + 2 if via_ir else 2 * x + 3, x + 2),
              ("uint64", "uint64"))
    check("storageEffects()", [], (2 if via_ir else 3, 2), ("uint64", "uint64"))
    for x in (-7, 0, 7):
        check("signedScalar(int16)", [x], (x if via_ir else 2 * x,), ("int16",))
    for run in (False, True):
        check("signedSeeded(bool)", [run], (-7 if run else (0 if via_ir else -5),), ("int16",))
