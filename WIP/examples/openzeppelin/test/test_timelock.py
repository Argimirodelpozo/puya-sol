"""
Timelock behavioral tests.
Tests scheduling, execution, cancellation, and delay management.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def ts_key(oid):
    return mapping_box_key("_opTimestamp", oid.to_bytes(64, "big"))


def ex_key(oid):
    return mapping_box_key("_opExecuted", oid.to_bytes(64, "big"))


def tgt_key(oid):
    return mapping_box_key("_opTarget", oid.to_bytes(64, "big"))


def val_key(oid):
    return mapping_box_key("_opValue", oid.to_bytes(64, "big"))


def op_boxes(oid):
    return [
        au.BoxReference(app_id=0, name=ts_key(oid)),
        au.BoxReference(app_id=0, name=ex_key(oid)),
        au.BoxReference(app_id=0, name=tgt_key(oid)),
        au.BoxReference(app_id=0, name=val_key(oid)),
    ]


@pytest.fixture(scope="module")
def timelock(localnet, account):
    return deploy_contract(localnet, account, "TimelockTest")


def test_deploy(timelock):
    assert timelock.app_id > 0


def test_admin(timelock, account):
    result = timelock.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_min_delay(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(method="getMinDelay")
    )
    assert result.abi_return == 10


def test_schedule_op(timelock):
    # Init boxes first, then schedule
    boxes = op_boxes(0)
    timelock.send.call(
        au.AppClientMethodCallParams(
            method="initOperation",
            args=[0],
            box_references=boxes,
        )
    )
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="schedule",
            args=[42, 100, 20, 1000],  # target=42, value=100, delay=20, currentTime=1000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # first operation id


def test_timestamp(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="getTimestamp",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=ts_key(0))],
        )
    )
    assert result.abi_return == 1020  # 1000 + 20


def test_not_ready_early(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="isOperationReady",
            args=[0, 1010],  # before timestamp
            box_references=[
                au.BoxReference(app_id=0, name=ts_key(0)),
                au.BoxReference(app_id=0, name=ex_key(0)),
            ],
        )
    )
    assert result.abi_return is False


def test_ready_after_delay(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="isOperationReady",
            args=[0, 1025],  # after timestamp
            box_references=[
                au.BoxReference(app_id=0, name=ts_key(0)),
                au.BoxReference(app_id=0, name=ex_key(0)),
            ],
        )
    )
    assert result.abi_return is True


def test_not_done(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="isOperationDone",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=ex_key(0))],
        )
    )
    assert result.abi_return is False


def test_execute(timelock):
    boxes = [
        au.BoxReference(app_id=0, name=ts_key(0)),
        au.BoxReference(app_id=0, name=ex_key(0)),
    ]
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="execute",
            args=[0, 1025],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_done_after_execute(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="isOperationDone",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=ex_key(0))],
            note=b"done2",
        )
    )
    assert result.abi_return is True


def test_schedule_and_cancel(timelock):
    boxes = op_boxes(1)
    timelock.send.call(
        au.AppClientMethodCallParams(
            method="initOperation",
            args=[1],
            box_references=boxes,
        )
    )
    timelock.send.call(
        au.AppClientMethodCallParams(
            method="schedule",
            args=[99, 200, 15, 2000],
            box_references=boxes,
            note=b"sched2",
        )
    )
    # Cancel it
    timelock.send.call(
        au.AppClientMethodCallParams(
            method="cancel",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=ts_key(1)),
                au.BoxReference(app_id=0, name=ex_key(1)),
            ],
        )
    )


def test_cancelled_timestamp(timelock):
    result = timelock.send.call(
        au.AppClientMethodCallParams(
            method="getTimestamp",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=ts_key(1))],
            note=b"ts2",
        )
    )
    assert result.abi_return == 0  # cancelled


def test_set_min_delay(timelock):
    timelock.send.call(
        au.AppClientMethodCallParams(
            method="setMinDelay",
            args=[50],
        )
    )
    result = timelock.send.call(
        au.AppClientMethodCallParams(method="getMinDelay", note=b"md2")
    )
    assert result.abi_return == 50
