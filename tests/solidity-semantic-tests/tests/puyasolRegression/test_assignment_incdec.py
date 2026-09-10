"""Assignment ordering and native inc/dec values, checked against both solc modes."""

from itertools import product

import pytest

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_assignment_order(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/assignment_order.sol", contract_name="AssignmentOrder",
        via_yul_behavior=via_ir, extra_args=["--evm-storage-layout"] if slot_layout else [])
    # Both solc code generators evaluate RHS before LHS, including the read
    # of the compound-assignment target. Memory write-backs must finish too.
    for compound, rhs_call in product((False, True), repeat=2):
        result = harness.call(app, "memoryOrder(bool,bool)", compound, rhs_call).abi_return
        rhs = 4 if rhs_call else 3
        initial = 20 if rhs_call else 7
        assert tuple(map(as_int, result)) == (9, rhs + (initial if compound else 0), 21 if rhs_call else 1)
    for compound in (False, True):
        result = harness.call(app, "storageOrder(bool)", compound).abi_return
        assert tuple(map(as_int, result)) == (3, 12 if compound else 5, 1)
        result = harness.call(app, "localIndex(bool)", compound).abi_return
        assert tuple(map(as_int, result)) == (11 if compound else 1, 2)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_incdec_values(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/incdec_values.sol", contract_name="IncDecValues",
        via_yul_behavior=via_ir, extra_args=["--evm-storage-layout"] if slot_layout else [])
    for x in (0, 10, 2**64 - 3):
        result = harness.call(app, "indexedValues(uint64)", x).abi_return
        assert list(map(as_int, result[0])) == [x + 1] * 5
        assert tuple(map(as_int, result[1:])) == (x + 1, 0)
    for x in (2, 2**64, 2**128 - 3):
        result = harness.call(app, "wide(uint128)", x).abi_return
        assert tuple(map(as_int, result)) == (x + 1, x + 1, x - 1, x - 1, x + 2, x - 2)
    for signed in (False, True):
        signature = "signedValue(int8,bool,bool,bool)" if signed else "narrow(uint8,bool,bool,bool)"
        low, high = (-128, 127) if signed else (0, 255)
        inputs = (low, -1, 0, high) if signed else (low, 1, high - 1, high)
        for x, dec, post, wrap in product(inputs, (False, True), (False, True), (False, True)):
            changed = x + (-1 if dec else 1)
            fail = not wrap and not low <= changed <= high
            result = harness.call(app, signature, x, dec, post, wrap, expect_revert=fail)
            if fail:
                assert result.reverted
            else:
                changed = (changed - low) % 256 + low
                decode = as_signed_int if signed else as_int
                assert tuple(map(decode, result.abi_return)) == (x if post else changed, changed)
