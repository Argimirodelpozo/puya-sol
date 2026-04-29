"""
EnumerableMap behavioral tests.
Tests set/get/remove/contains/at/length/tryGet operations.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def val_key(k):
    return mapping_box_key("_values", k.to_bytes(64, "big"))


def idx_key(k):
    return mapping_box_key("_indexOf", k.to_bytes(64, "big"))


def key_at_key(idx):
    return mapping_box_key("_keyAtIndex", idx.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def emap(localnet, account):
    return deploy_contract(localnet, account, "EnumerableMapTest")


def test_deploy(emap):
    assert emap.app_id > 0


def test_initial_length(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(method="length")
    )
    assert result.abi_return == 0


def test_set_first(emap):
    boxes = [
        au.BoxReference(app_id=0, name=val_key(10)),
        au.BoxReference(app_id=0, name=idx_key(10)),
        au.BoxReference(app_id=0, name=key_at_key(1)),
    ]
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="set", args=[10, 100], box_references=boxes,
        )
    )
    assert result.abi_return is True  # new entry


def test_set_second(emap):
    boxes = [
        au.BoxReference(app_id=0, name=val_key(20)),
        au.BoxReference(app_id=0, name=idx_key(20)),
        au.BoxReference(app_id=0, name=key_at_key(2)),
    ]
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="set", args=[20, 200], box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_length_after_set(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(method="length", note=b"l2")
    )
    assert result.abi_return == 2


def test_contains(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="contains", args=[10],
            box_references=[au.BoxReference(app_id=0, name=idx_key(10))],
        )
    )
    assert result.abi_return is True


def test_not_contains(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="contains", args=[99],
            box_references=[au.BoxReference(app_id=0, name=idx_key(99))],
        )
    )
    assert result.abi_return is False


def test_get(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="get", args=[10],
            box_references=[
                au.BoxReference(app_id=0, name=idx_key(10)),
                au.BoxReference(app_id=0, name=val_key(10)),
            ],
        )
    )
    assert result.abi_return == 100


def test_try_get_exists(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="tryGet", args=[20],
            box_references=[
                au.BoxReference(app_id=0, name=idx_key(20)),
                au.BoxReference(app_id=0, name=val_key(20)),
            ],
        )
    )
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["exists"] is True
        assert ret["value"] == 200
    else:
        assert ret[0] is True
        assert ret[1] == 200


def test_try_get_missing(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="tryGet", args=[99],
            box_references=[
                au.BoxReference(app_id=0, name=idx_key(99)),
                au.BoxReference(app_id=0, name=val_key(99)),
            ],
        )
    )
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["exists"] is False
    else:
        assert ret[0] is False


def test_at_index(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="at", args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=key_at_key(1)),
                au.BoxReference(app_id=0, name=val_key(10)),
            ],
        )
    )
    ret = result.abi_return
    # May be dict or list depending on struct mapping
    vals = list(ret.values()) if isinstance(ret, dict) else list(ret)
    assert vals[0] == 10
    assert vals[1] == 100


def test_update_existing(emap):
    boxes = [
        au.BoxReference(app_id=0, name=val_key(10)),
        au.BoxReference(app_id=0, name=idx_key(10)),
        au.BoxReference(app_id=0, name=key_at_key(1)),
    ]
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="set", args=[10, 999], box_references=boxes,
        )
    )
    assert result.abi_return is False  # updated, not new


def test_get_after_update(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="get", args=[10],
            box_references=[
                au.BoxReference(app_id=0, name=idx_key(10)),
                au.BoxReference(app_id=0, name=val_key(10)),
            ],
            note=b"after_update",
        )
    )
    assert result.abi_return == 999


def test_remove(emap):
    boxes = [
        au.BoxReference(app_id=0, name=idx_key(10)),
        au.BoxReference(app_id=0, name=idx_key(20)),
        au.BoxReference(app_id=0, name=key_at_key(1)),
        au.BoxReference(app_id=0, name=key_at_key(2)),
        au.BoxReference(app_id=0, name=val_key(10)),
    ]
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="remove", args=[10], box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_length_after_remove(emap):
    result = emap.send.call(
        au.AppClientMethodCallParams(method="length", note=b"l3")
    )
    assert result.abi_return == 1


def test_remove_nonexistent(emap):
    boxes = [
        au.BoxReference(app_id=0, name=idx_key(99)),
        au.BoxReference(app_id=0, name=key_at_key(0)),
        au.BoxReference(app_id=0, name=val_key(99)),
    ]
    result = emap.send.call(
        au.AppClientMethodCallParams(
            method="remove", args=[99], box_references=boxes,
        )
    )
    assert result.abi_return is False
