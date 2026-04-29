"""
Counters behavioral tests.
Tests increment, decrement, current, and reset operations.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def counter(localnet, account):
    return deploy_contract(localnet, account, "CountersTest")


def test_deploy(counter):
    assert counter.app_id > 0


def test_initial_value_zero(counter):
    result = counter.send.call(
        au.AppClientMethodCallParams(method="current")
    )
    assert result.abi_return == 0


def test_increment(counter):
    counter.send.call(
        au.AppClientMethodCallParams(method="increment")
    )
    result = counter.send.call(
        au.AppClientMethodCallParams(method="current")
    )
    assert result.abi_return == 1


def test_increment_twice(counter):
    counter.send.call(
        au.AppClientMethodCallParams(method="increment", note=b"inc2")
    )
    result = counter.send.call(
        au.AppClientMethodCallParams(method="current", note=b"cur2")
    )
    assert result.abi_return == 2


def test_decrement(counter):
    counter.send.call(
        au.AppClientMethodCallParams(method="decrement")
    )
    result = counter.send.call(
        au.AppClientMethodCallParams(method="current", note=b"cur3")
    )
    assert result.abi_return == 1


def test_reset(counter):
    counter.send.call(
        au.AppClientMethodCallParams(method="reset")
    )
    result = counter.send.call(
        au.AppClientMethodCallParams(method="current", note=b"cur4")
    )
    assert result.abi_return == 0


def test_decrement_at_zero_reverts(counter):
    """Decrement at zero should revert."""
    with pytest.raises(Exception):
        counter.send.call(
            au.AppClientMethodCallParams(method="decrement", note=b"fail")
        )
