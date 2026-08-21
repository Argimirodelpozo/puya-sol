"""Regression guards for inner-call returns and library memory writeback."""

from framework import as_int


def _ints(values):
    return tuple(as_int(value) for value in values)


def test_try_success_return_bindings_follow_inner_call(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_b1_try_returns.sol",
        contract_name="TryReturnsCaller")

    assert as_int(harness.call(
        app, "one(uint256)", 10, extra_fee=10_000).abi_return) == 18
    assert as_int(harness.call(
        app, "pair(uint256)", 10, extra_fee=10_000).abi_return) == 1112
    assert as_int(harness.call(
        app, "noReturns()", extra_fee=10_000).abi_return) == 123


def test_public_library_memory_parameter_writeback(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_b2_public_library_memory.sol",
        contract_name="PublicLibraryMemoryCaller")

    assert _ints(harness.call(app, "direct(uint256)", 40).abi_return) == (40, 41)
    assert _ints(harness.call(app, "attached(uint256)", 50).abi_return) == (50, 51)
    assert _ints(harness.call(app, "withReturn(uint256)", 60).abi_return) \
        == (121, 60, 61)
    assert _ints(harness.call(
        app, "externalVisibility(uint256)", 70).abi_return) == (70, 71)
