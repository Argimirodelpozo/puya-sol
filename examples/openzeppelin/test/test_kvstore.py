"""
KVStore behavioral tests.
Tests key-value operations, versioning, and batch operations.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def val_key(k):
    return mapping_box_key("_values", k.to_bytes(64, "big"))


def exists_key(k):
    return mapping_box_key("_exists", k.to_bytes(64, "big"))


def modified_key(k):
    return mapping_box_key("_lastModified", k.to_bytes(64, "big"))


def key_boxes(k):
    return [
        au.BoxReference(app_id=0, name=val_key(k)),
        au.BoxReference(app_id=0, name=exists_key(k)),
        au.BoxReference(app_id=0, name=modified_key(k)),
    ]


@pytest.fixture(scope="module")
def kv(localnet, account):
    return deploy_contract(localnet, account, "KVStoreTest")


def test_deploy(kv):
    assert kv.app_id > 0


def test_admin(kv, account):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_initial_state(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getEntryCount")
    )
    assert result.abi_return == 0
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getVersion")
    )
    assert result.abi_return == 0


def test_set_value(kv):
    boxes = key_boxes(1)
    kv.send.call(
        au.AppClientMethodCallParams(
            method="initKey",
            args=[1],
            box_references=boxes,
        )
    )
    kv.send.call(
        au.AppClientMethodCallParams(
            method="setValue",
            args=[1, 42],
            box_references=boxes,
        )
    )


def test_get_value(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=val_key(1))],
        )
    )
    assert result.abi_return == 42


def test_exists(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(
            method="exists",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=exists_key(1))],
        )
    )
    assert result.abi_return is True


def test_entry_count(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getEntryCount")
    )
    assert result.abi_return == 1


def test_version_after_set(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getVersion")
    )
    assert result.abi_return == 1


def test_last_modified(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(
            method="getLastModified",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=modified_key(1))],
        )
    )
    assert result.abi_return == 0  # version was 0 when set


def test_update_value(kv):
    kv.send.call(
        au.AppClientMethodCallParams(
            method="setValue",
            args=[1, 99],
            box_references=key_boxes(1),
            note=b"sv2",
        )
    )
    result = kv.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=val_key(1))],
            note=b"gv2",
        )
    )
    assert result.abi_return == 99


def test_entry_count_unchanged(kv):
    # Updating existing key shouldn't increment count
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getEntryCount", note=b"ec2")
    )
    assert result.abi_return == 1


def test_version_after_update(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getVersion", note=b"v2")
    )
    assert result.abi_return == 2


def test_total_writes(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getTotalWrites")
    )
    assert result.abi_return == 2


def test_delete_key(kv):
    kv.send.call(
        au.AppClientMethodCallParams(
            method="deleteKey",
            args=[1],
            box_references=key_boxes(1),
        )
    )


def test_not_exists_after_delete(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(
            method="exists",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=exists_key(1))],
            note=b"ex2",
        )
    )
    assert result.abi_return is False


def test_entry_count_after_delete(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getEntryCount", note=b"ec3")
    )
    assert result.abi_return == 0


def test_total_deletes(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getTotalDeletes")
    )
    assert result.abi_return == 1


def test_batch_set(kv):
    # Init key 10 and 20
    for k in [10, 20]:
        kv.send.call(
            au.AppClientMethodCallParams(
                method="initKey",
                args=[k],
                box_references=key_boxes(k),
            )
        )
    kv.send.call(
        au.AppClientMethodCallParams(
            method="batchSet",
            args=[10, 100, 20, 200],
            box_references=key_boxes(10) + key_boxes(20),
        )
    )


def test_batch_values(kv):
    r1 = kv.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[10],
            box_references=[au.BoxReference(app_id=0, name=val_key(10))],
        )
    )
    assert r1.abi_return == 100
    r2 = kv.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[20],
            box_references=[au.BoxReference(app_id=0, name=val_key(20))],
        )
    )
    assert r2.abi_return == 200


def test_entry_count_after_batch(kv):
    result = kv.send.call(
        au.AppClientMethodCallParams(method="getEntryCount", note=b"ec4")
    )
    assert result.abi_return == 2
