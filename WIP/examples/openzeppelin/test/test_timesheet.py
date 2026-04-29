"""
Timesheet behavioral tests.
Tests project creation, hour logging, and deactivation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def name_key(pid):
    return mapping_box_key("_projectName", pid.to_bytes(64, "big"))
def active_key(pid):
    return mapping_box_key("_projectActive", pid.to_bytes(64, "big"))
def proj_hours_key(pid):
    return mapping_box_key("_projectHours", pid.to_bytes(64, "big"))
def worker_hours_key(addr):
    return mapping_box_key("_workerHours", encoding.decode_address(addr))
def worker_idx_key(addr):
    return mapping_box_key("_workerIndex", encoding.decode_address(addr))

def proj_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=name_key(pid)),
        au.BoxReference(app_id=0, name=active_key(pid)),
        au.BoxReference(app_id=0, name=proj_hours_key(pid)),
    ]

@pytest.fixture(scope="module")
def ts(localnet, account):
    return deploy_contract(localnet, account, "TimesheetTest")

def test_deploy(ts):
    assert ts.app_id > 0

def test_admin(ts, account):
    r = ts.send.call(au.AppClientMethodCallParams(method="admin"))
    assert r.abi_return == account.address

def test_create_project(ts):
    boxes = proj_boxes(0)
    ts.send.call(au.AppClientMethodCallParams(
        method="initProject", args=[0], box_references=boxes))
    r = ts.send.call(au.AppClientMethodCallParams(
        method="createProject", box_references=boxes))
    assert r.abi_return == 0

def test_project_count(ts):
    r = ts.send.call(au.AppClientMethodCallParams(method="projectCount"))
    assert r.abi_return == 1

def test_project_active(ts):
    r = ts.send.call(au.AppClientMethodCallParams(
        method="isProjectActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_init_worker_and_log(ts, account):
    ts.send.call(au.AppClientMethodCallParams(
        method="initWorker", args=[account.address],
        box_references=[
            au.BoxReference(app_id=0, name=worker_hours_key(account.address)),
            au.BoxReference(app_id=0, name=worker_idx_key(account.address)),
        ]))
    ts.send.call(au.AppClientMethodCallParams(
        method="logHours", args=[0, account.address, 8],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=proj_hours_key(0)),
            au.BoxReference(app_id=0, name=worker_hours_key(account.address)),
        ]))

def test_project_hours(ts):
    r = ts.send.call(au.AppClientMethodCallParams(
        method="getProjectHours", args=[0],
        box_references=[au.BoxReference(app_id=0, name=proj_hours_key(0))]))
    assert r.abi_return == 8

def test_worker_hours(ts, account):
    r = ts.send.call(au.AppClientMethodCallParams(
        method="getWorkerHours", args=[account.address],
        box_references=[au.BoxReference(app_id=0, name=worker_hours_key(account.address))]))
    assert r.abi_return == 8

def test_total_hours(ts):
    r = ts.send.call(au.AppClientMethodCallParams(method="totalHoursLogged"))
    assert r.abi_return == 8

def test_log_more(ts, account):
    ts.send.call(au.AppClientMethodCallParams(
        method="logHours", args=[0, account.address, 4],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=proj_hours_key(0)),
            au.BoxReference(app_id=0, name=worker_hours_key(account.address)),
        ], note=b"l2"))

def test_total_hours_after(ts):
    r = ts.send.call(au.AppClientMethodCallParams(method="totalHoursLogged", note=b"t2"))
    assert r.abi_return == 12

def test_deactivate(ts):
    ts.send.call(au.AppClientMethodCallParams(
        method="deactivateProject", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = ts.send.call(au.AppClientMethodCallParams(
        method="isProjectActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))], note=b"a2"))
    assert r.abi_return is False

def test_activate(ts):
    ts.send.call(au.AppClientMethodCallParams(
        method="activateProject", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = ts.send.call(au.AppClientMethodCallParams(
        method="isProjectActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))], note=b"a3"))
    assert r.abi_return is True
