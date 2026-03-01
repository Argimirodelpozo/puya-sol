"""
TokenLock behavioral tests.
Tests lock creation, release, and time checks.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def owner_key(lid):
    return mapping_box_key("_lockOwner", lid.to_bytes(64, "big"))


def amount_key(lid):
    return mapping_box_key("_lockAmount", lid.to_bytes(64, "big"))


def time_key(lid):
    return mapping_box_key("_lockReleaseTime", lid.to_bytes(64, "big"))


def released_key(lid):
    return mapping_box_key("_lockReleased", lid.to_bytes(64, "big"))


def lock_boxes(lid):
    return [
        au.BoxReference(app_id=0, name=owner_key(lid)),
        au.BoxReference(app_id=0, name=amount_key(lid)),
        au.BoxReference(app_id=0, name=time_key(lid)),
        au.BoxReference(app_id=0, name=released_key(lid)),
    ]


@pytest.fixture(scope="module")
def tlock(localnet, account):
    return deploy_contract(localnet, account, "TokenLockTest")


def test_deploy(tlock):
    assert tlock.app_id > 0


def test_admin(tlock, account):
    result = tlock.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_create_lock(tlock, account):
    boxes = lock_boxes(0)
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="createLock",
            args=[account.address, 5000, 2000],  # amount=5000, releaseTime=2000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_lock_owner(tlock, account):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="getLockOwner",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=owner_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_lock_amount(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="getLockAmount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
        )
    )
    assert result.abi_return == 5000


def test_release_time(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="getReleaseTime",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=time_key(0))],
        )
    )
    assert result.abi_return == 2000


def test_not_released(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="isReleased",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=released_key(0))],
        )
    )
    assert result.abi_return is False


def test_not_releasable_early(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="isReleasable",
            args=[0, 1500],
            box_references=[
                au.BoxReference(app_id=0, name=released_key(0)),
                au.BoxReference(app_id=0, name=time_key(0)),
            ],
        )
    )
    assert result.abi_return is False


def test_releasable_after_time(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="isReleasable",
            args=[0, 2500],
            box_references=[
                au.BoxReference(app_id=0, name=released_key(0)),
                au.BoxReference(app_id=0, name=time_key(0)),
            ],
        )
    )
    assert result.abi_return is True


def test_total_locked(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(method="getTotalLocked")
    )
    assert result.abi_return == 5000


def test_release(tlock):
    tlock.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[0, 2500],
            box_references=[
                au.BoxReference(app_id=0, name=released_key(0)),
                au.BoxReference(app_id=0, name=time_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
            ],
        )
    )


def test_released_after(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="isReleased",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=released_key(0))],
            note=b"rel2",
        )
    )
    assert result.abi_return is True


def test_total_released(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(method="getTotalReleased")
    )
    assert result.abi_return == 5000


def test_create_second_lock(tlock, account):
    boxes = lock_boxes(1)
    result = tlock.send.call(
        au.AppClientMethodCallParams(
            method="createLock",
            args=[account.address, 3000, 9000],
            box_references=boxes,
            note=b"lock2",
        )
    )
    assert result.abi_return == 1


def test_lock_count(tlock):
    result = tlock.send.call(
        au.AppClientMethodCallParams(method="getLockCount")
    )
    assert result.abi_return == 2
