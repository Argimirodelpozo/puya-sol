"""
AuditTrail behavioral tests.
Tests audit event recording and querying.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def action_key(aid):
    return mapping_box_key("_auditAction", aid.to_bytes(64, "big"))
def actor_key(aid):
    return mapping_box_key("_auditActor", aid.to_bytes(64, "big"))
def ts_key(aid):
    return mapping_box_key("_auditTimestamp", aid.to_bytes(64, "big"))

def audit_boxes(aid):
    return [
        au.BoxReference(app_id=0, name=action_key(aid)),
        au.BoxReference(app_id=0, name=actor_key(aid)),
        au.BoxReference(app_id=0, name=ts_key(aid)),
    ]

@pytest.fixture(scope="module")
def at(localnet, account):
    return deploy_contract(localnet, account, "AuditTrailTest")

def test_deploy(at):
    assert at.app_id > 0

def test_admin(at, account):
    r = at.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_record_audit(at):
    boxes = audit_boxes(0)
    at.send.call(au.AppClientMethodCallParams(
        method="initAudit", args=[0], box_references=boxes))
    r = at.send.call(au.AppClientMethodCallParams(
        method="recordAudit", args=[1, 100, 5000],
        box_references=boxes))
    assert r.abi_return == 0

def test_audit_count(at):
    r = at.send.call(au.AppClientMethodCallParams(method="getAuditCount"))
    assert r.abi_return == 1

def test_audit_action(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAuditAction", args=[0],
        box_references=[au.BoxReference(app_id=0, name=action_key(0))]))
    assert r.abi_return == 1

def test_audit_actor(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAuditActor", args=[0],
        box_references=[au.BoxReference(app_id=0, name=actor_key(0))]))
    assert r.abi_return == 100

def test_audit_timestamp(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAuditTimestamp", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ts_key(0))]))
    assert r.abi_return == 5000

def test_record_second(at):
    boxes = audit_boxes(1)
    at.send.call(au.AppClientMethodCallParams(
        method="initAudit", args=[1], box_references=boxes))
    r = at.send.call(au.AppClientMethodCallParams(
        method="recordAudit", args=[2, 200, 6000],
        box_references=boxes, note=b"a2"))
    assert r.abi_return == 1

def test_record_third(at):
    boxes = audit_boxes(2)
    at.send.call(au.AppClientMethodCallParams(
        method="initAudit", args=[2], box_references=boxes))
    at.send.call(au.AppClientMethodCallParams(
        method="recordAudit", args=[3, 300, 7000],
        box_references=boxes, note=b"a3"))

def test_audit_count_final(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAuditCount", note=b"c2"))
    assert r.abi_return == 3

def test_third_action(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAuditAction", args=[2],
        box_references=[au.BoxReference(app_id=0, name=action_key(2))]))
    assert r.abi_return == 3
