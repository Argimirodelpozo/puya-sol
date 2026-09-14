"""Dominating signed intermediates must retain Solidity/Yul arithmetic policy."""

import pytest

from test_call_operands import invoke

SOURCE = "puyasolRegression/contracts/reduction_arithmetic.sol"


def quotient(x, y):
    return abs(x) // abs(y) * (-1 if (x < 0) != (y < 0) else 1)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_reduction_signed_arithmetic(harness, via_ir):
    artifacts = harness.compile(SOURCE, via_yul_behavior=via_ir, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "ReductionArithmetic")
    for bits in (64, 128, 256):
        minimum, maximum = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        pairs = [(x, y) for x in (-7, 0, 7) for y in (-9, -3, 3, 9)]
        pairs += [(minimum, 1), (minimum, 2), (minimum, -2), (maximum, -1), (minimum, minimum)]
        for x, y in pairs:
            q = quotient(x, y)
            for method, expected in (("divide", q), ("modulo", x - q * y), ("unchecked", q)):
                assert invoke(harness, app, "evm", f"{method}{bits}(int{bits},int{bits})",
                              [x, y], [f"int{bits}"]) == (expected,)
        for method in ("divide", "modulo", "unchecked"):
            invoke(harness, app, "evm", f"{method}{bits}(int{bits},int{bits})", [1, 0], reverts=True)
        invoke(harness, app, "evm", f"divide{bits}(int{bits},int{bits})", [minimum, -1], reverts=True)
        assert invoke(harness, app, "evm", f"unchecked{bits}(int{bits},int{bits})",
                      [minimum, -1], [f"int{bits}"]) == (minimum,)
        assert invoke(harness, app, "evm", f"modulo{bits}(int{bits},int{bits})",
                      [minimum, -1], [f"int{bits}"]) == (0,)
    word = 1 << 256
    for x, y in [(-7, 3), (7, -3), (-7, -3), (-1, 9), (0, -3), (5, 0), (-(1 << 255), -1)]:
        q = quotient(x, y) if y else 0
        r = x - q * y if y else 0
        assert invoke(harness, app, "evm", "yul(uint256,uint256)", [x % word, y % word],
                      ["uint256", "uint256"]) == (q % word, r % word)
    for count in (2, 4):
        assert invoke(harness, app, "evm", "effects(int64,int64)", [-7, 3],
                      ["int64", "uint256"]) == (-2, count)
    teal = artifacts.by_contract["ReductionDivideOnly"]["approval_teal"].read_text()
    assert sum(line.strip() == "b/" for line in teal.splitlines()) == 1
