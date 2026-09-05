"""Public array getters must revert on an out-of-range index.

Solidity's generated getter indexes the array, so an out-of-range read is
Panic(0x32). Only the mapping-key levels were bounds-checked; a plain index
argument read inside the already-loaded value unchecked and returned decoded
garbage (found on Privacy Pools' associationSets(uint256), where the EVM
reverted and the AVM answered).
"""
import pytest

SOURCE = "puyasolRegression/contracts/getter_array_bounds.sol"


@pytest.fixture(params=[False, True], ids=["named", "slots"])
def app(harness, request):
    a = harness.compile_and_deploy(
        SOURCE, "GetterBounds", extra_args=["--evm-storage-layout"] if request.param else [])
    r = harness.call(a, "seed()", extra_fee=20_000)
    assert not r.reverted, r.fail_message
    return a


def _call(harness, app, sig, *args):
    return harness.call(app, sig, *args, extra_fee=20_000)


def test_in_range_reads_work(harness, app):
    assert _call(harness, app, "nums(uint256)", 0).abi_return == 7
    assert _call(harness, app, "nums(uint256)", 1).abi_return == 8
    assert _call(harness, app, "sets(uint256)", 0).abi_return == [11, "x", 22]
    assert _call(harness, app, "nested(uint256,uint256)", 5, 0).abi_return == 99
    assert _call(harness, app, "matrix(uint256,uint256)", 1, 0).abi_return == 31
    assert _call(harness, app, "keyed(uint256,uint256)", 1, 5).abi_return == 41
    assert _call(harness, app, "fixedNested(uint256,uint256)", 5, 0).abi_return == 51


def test_value_array_out_of_range_reverts(harness, app):
    assert _call(harness, app, "nums(uint256)", 2).reverted
    assert _call(harness, app, "nums(uint256)", 100).reverted


def test_struct_array_out_of_range_reverts(harness, app):
    assert _call(harness, app, "sets(uint256)", 1).reverted
    assert _call(harness, app, "sets(uint256)", 32).reverted


def test_fixed_array_out_of_range_reverts(harness, app):
    assert not _call(harness, app, "fixedNums(uint256)", 2).reverted
    assert _call(harness, app, "fixedNums(uint256)", 3).reverted


def test_mapping_to_array_out_of_range_reverts(harness, app):
    assert _call(harness, app, "nested(uint256,uint256)", 5, 1).reverted
    # An absent mapping key yields an empty array — still out of range.
    assert _call(harness, app, "nested(uint256,uint256)", 9, 0).reverted


@pytest.mark.parametrize("method,args", [
    ("nums(uint256)", ()),
    ("sets(uint256)", ()),
    ("fixedNums(uint256)", ()),
    ("nested(uint256,uint256)", (5,)),
    ("matrix(uint256,uint256)", (1,)),
    ("fixedNested(uint256,uint256)", (5,)),
])
def test_full_width_getter_index_is_checked(harness, app, method, args):
    for index in (1 << 64, (1 << 128) + 1, (1 << 256) - 1):
        assert _call(harness, app, method, *args, index).reverted


@pytest.mark.parametrize("method,last", [
    ("matrix(uint256,uint256)", 0),
    ("keyed(uint256,uint256)", 5),
])
def test_full_width_outer_getter_index_is_checked(harness, app, method, last):
    for index in (2, 1 << 64, (1 << 128) + 1, (1 << 256) - 1):
        assert _call(harness, app, method, index, last).reverted
