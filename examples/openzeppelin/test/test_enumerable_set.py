"""
EnumerableSet behavioral tests.
Tests add/remove/contains/length for a simplified set implementation.
"""
import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def contains_key(value):
    """Box key for _contains mapping."""
    return mapping_box_key("_contains", value.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def eset(localnet, account):
    return deploy_contract(localnet, account, "EnumerableSetTest")


def test_deploy(eset):
    assert eset.app_id > 0


def test_initially_empty(eset):
    result = eset.send.call(
        au.AppClientMethodCallParams(method="length")
    )
    assert result.abi_return == 0


def test_add_element(eset):
    key = contains_key(42)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="add", args=[42],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is True


def test_contains_after_add(eset):
    key = contains_key(42)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="contains", args=[42],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is True


def test_length_after_add(eset):
    result = eset.send.call(
        au.AppClientMethodCallParams(method="length")
    )
    assert result.abi_return == 1


def test_add_duplicate_returns_false(eset):
    key = contains_key(42)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="add", args=[42],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"dup",
        )
    )
    assert result.abi_return is False


def test_add_second_element(eset):
    key = contains_key(100)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="add", args=[100],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is True


def test_length_after_two_adds(eset):
    result = eset.send.call(
        au.AppClientMethodCallParams(method="length")
    )
    assert result.abi_return == 2


def test_remove_element(eset):
    key = contains_key(42)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="remove", args=[42],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is True


def test_contains_after_remove(eset):
    key = contains_key(42)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="contains", args=[42],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is False


def test_length_after_remove(eset):
    result = eset.send.call(
        au.AppClientMethodCallParams(method="length")
    )
    assert result.abi_return == 1


def test_remove_nonexistent_returns_false(eset):
    key = contains_key(999)
    result = eset.send.call(
        au.AppClientMethodCallParams(
            method="remove", args=[999],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"nonexist",
        )
    )
    assert result.abi_return is False
