"""
AccessLog behavioral tests.
Tests access logging, denied logging, and counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def ts_key(lid):
    return mapping_box_key("_logTimestamp", lid.to_bytes(64, "big"))
def res_key(lid):
    return mapping_box_key("_logResource", lid.to_bytes(64, "big"))
def denied_key(lid):
    return mapping_box_key("_logDenied", lid.to_bytes(64, "big"))

def log_boxes(lid):
    return [
        au.BoxReference(app_id=0, name=ts_key(lid)),
        au.BoxReference(app_id=0, name=res_key(lid)),
        au.BoxReference(app_id=0, name=denied_key(lid)),
    ]

@pytest.fixture(scope="module")
def al(localnet, account):
    return deploy_contract(localnet, account, "AccessLogTest")

def test_deploy(al):
    assert al.app_id > 0

def test_admin(al, account):
    r = al.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_log_access(al):
    boxes = log_boxes(0)
    al.send.call(au.AppClientMethodCallParams(
        method="initLog", args=[0], box_references=boxes))
    r = al.send.call(au.AppClientMethodCallParams(
        method="logAccess", args=[100, 1000],
        box_references=boxes))
    assert r.abi_return == 0

def test_log_count(al):
    r = al.send.call(au.AppClientMethodCallParams(method="getLogCount"))
    assert r.abi_return == 1

def test_log_timestamp(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getLogTimestamp", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ts_key(0))]))
    assert r.abi_return == 1000

def test_log_resource(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getLogResource", args=[0],
        box_references=[au.BoxReference(app_id=0, name=res_key(0))]))
    assert r.abi_return == 100

def test_not_denied(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="isLogDenied", args=[0],
        box_references=[au.BoxReference(app_id=0, name=denied_key(0))]))
    assert r.abi_return is False

def test_denied_count_initial(al):
    r = al.send.call(au.AppClientMethodCallParams(method="getDeniedCount"))
    assert r.abi_return == 0

def test_log_denied(al):
    boxes = log_boxes(1)
    al.send.call(au.AppClientMethodCallParams(
        method="initLog", args=[1], box_references=boxes))
    r = al.send.call(au.AppClientMethodCallParams(
        method="logDenied", args=[200, 2000],
        box_references=boxes))
    assert r.abi_return == 1

def test_log_count_after(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getLogCount", note=b"lc2"))
    assert r.abi_return == 2

def test_denied_count_after(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getDeniedCount", note=b"dc2"))
    assert r.abi_return == 1

def test_is_denied(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="isLogDenied", args=[1],
        box_references=[au.BoxReference(app_id=0, name=denied_key(1))]))
    assert r.abi_return is True

def test_log_more_access(al):
    boxes = log_boxes(2)
    al.send.call(au.AppClientMethodCallParams(
        method="initLog", args=[2], box_references=boxes))
    al.send.call(au.AppClientMethodCallParams(
        method="logAccess", args=[300, 3000],
        box_references=boxes, note=b"a2"))

def test_log_count_final(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getLogCount", note=b"lc3"))
    assert r.abi_return == 3

def test_denied_count_final(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getDeniedCount", note=b"dc3"))
    assert r.abi_return == 1
