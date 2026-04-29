"""
TaskManager behavioral tests.
Tests task creation, assignment, completion, and status tracking.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def priority_key(tid):
    return mapping_box_key("_taskPriority", tid.to_bytes(64, "big"))

def effort_key(tid):
    return mapping_box_key("_taskEffort", tid.to_bytes(64, "big"))

def assignee_key(tid):
    return mapping_box_key("_taskAssignee", tid.to_bytes(64, "big"))

def status_key(tid):
    return mapping_box_key("_taskStatus", tid.to_bytes(64, "big"))

def exists_key(tid):
    return mapping_box_key("_taskExists", tid.to_bytes(64, "big"))

def task_boxes(tid):
    return [
        au.BoxReference(app_id=0, name=priority_key(tid)),
        au.BoxReference(app_id=0, name=effort_key(tid)),
        au.BoxReference(app_id=0, name=assignee_key(tid)),
        au.BoxReference(app_id=0, name=status_key(tid)),
        au.BoxReference(app_id=0, name=exists_key(tid)),
    ]


@pytest.fixture(scope="module")
def tm(localnet, account):
    return deploy_contract(localnet, account, "TaskManagerTest")


def test_deploy(tm):
    assert tm.app_id > 0

def test_admin(tm, account):
    r = tm.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_task(tm):
    boxes = task_boxes(0)
    r = tm.send.call(au.AppClientMethodCallParams(
        method="createTask", args=[2, 10], box_references=boxes))
    assert r.abi_return == 0

def test_task_count(tm):
    r = tm.send.call(au.AppClientMethodCallParams(method="getTaskCount"))
    assert r.abi_return == 1

def test_task_priority(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="getTaskPriority", args=[0],
        box_references=[au.BoxReference(app_id=0, name=priority_key(0))]))
    assert r.abi_return == 2

def test_task_effort(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="getTaskEffort", args=[0],
        box_references=[au.BoxReference(app_id=0, name=effort_key(0))]))
    assert r.abi_return == 10

def test_task_exists(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="taskExists", args=[0],
        box_references=[au.BoxReference(app_id=0, name=exists_key(0))]))
    assert r.abi_return is True

def test_task_status_open(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="getTaskStatus", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return == 0

def test_assign_task(tm, account):
    tm.send.call(au.AppClientMethodCallParams(
        method="assignTask", args=[0, account.address],
        box_references=[
            au.BoxReference(app_id=0, name=exists_key(0)),
            au.BoxReference(app_id=0, name=status_key(0)),
            au.BoxReference(app_id=0, name=assignee_key(0)),
        ]))

def test_task_status_assigned(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="getTaskStatus", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))],
        note=b"s2"))
    assert r.abi_return == 1

def test_task_assignee(tm, account):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="getTaskAssignee", args=[0],
        box_references=[au.BoxReference(app_id=0, name=assignee_key(0))]))
    assert r.abi_return == account.address

def test_complete_task(tm):
    tm.send.call(au.AppClientMethodCallParams(
        method="completeTask", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=exists_key(0)),
            au.BoxReference(app_id=0, name=status_key(0)),
        ]))

def test_is_complete(tm):
    r = tm.send.call(au.AppClientMethodCallParams(
        method="isComplete", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return is True

def test_completed_count(tm):
    r = tm.send.call(au.AppClientMethodCallParams(method="getCompletedCount"))
    assert r.abi_return == 1

def test_total_effort(tm):
    r = tm.send.call(au.AppClientMethodCallParams(method="getTotalEffort"))
    assert r.abi_return == 10

def test_create_second(tm):
    boxes = task_boxes(1)
    r = tm.send.call(au.AppClientMethodCallParams(
        method="createTask", args=[0, 5], box_references=boxes, note=b"t2"))
    assert r.abi_return == 1
