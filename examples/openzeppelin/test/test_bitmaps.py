"""
OpenZeppelin BitMaps behavioral tests.
Tests bit manipulation (get/set/unset/setTo).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def data_key(bucket: int) -> bytes:
    """Box key for _data[bucket]."""
    bucket_bytes = bucket.to_bytes(64, "big")
    return mapping_box_key("_data", bucket_bytes)


@pytest.fixture(scope="module")
def bitmaps(localnet, account):
    return deploy_contract(localnet, account, "BitMapsTest")


def test_deploy(bitmaps):
    assert bitmaps.app_id > 0


def test_initially_unset(bitmaps):
    key = data_key(0)  # bucket 0 for index 0-255
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[0],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is False


def test_set_bit(bitmaps):
    key = data_key(0)
    bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="set",
            args=[42],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[42],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is True


def test_other_bits_unaffected(bitmaps):
    key = data_key(0)
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[41],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is False


def test_unset_bit(bitmaps):
    key = data_key(0)
    bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="unset",
            args=[42],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[42],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is False


def test_set_to_true(bitmaps):
    key = data_key(0)
    bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="setTo",
            args=[100, True],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[100],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is True


def test_set_to_false(bitmaps):
    key = data_key(0)
    bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="setTo",
            args=[100, False],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[100],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is False


def test_different_bucket(bitmaps):
    """Index 256 should be in bucket 1."""
    key = data_key(1)
    bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="set",
            args=[256],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    result = bitmaps.send.call(
        au.AppClientMethodCallParams(
            method="get",
            args=[256],
            box_references=[box_ref(bitmaps.app_id, key)],
        )
    )
    assert result.abi_return is True


def test_set_multiple_bits(bitmaps):
    key = data_key(0)
    for i in [0, 1, 255]:
        bitmaps.send.call(
            au.AppClientMethodCallParams(
                method="set",
                args=[i],
                box_references=[box_ref(bitmaps.app_id, key)],
            )
        )
    for i in [0, 1, 255]:
        result = bitmaps.send.call(
            au.AppClientMethodCallParams(
                method="get",
                args=[i],
                box_references=[box_ref(bitmaps.app_id, key)],
            )
        )
        assert result.abi_return is True
