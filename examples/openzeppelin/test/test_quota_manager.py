"""
QuotaManager behavioral tests.
Tests quota creation, usage, remaining calculation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def limit_key(qid):
    return mapping_box_key("_quotaLimit", qid.to_bytes(64, "big"))
def used_key(qid):
    return mapping_box_key("_quotaUsed", qid.to_bytes(64, "big"))

def quota_boxes(qid):
    return [
        au.BoxReference(app_id=0, name=limit_key(qid)),
        au.BoxReference(app_id=0, name=used_key(qid)),
    ]

@pytest.fixture(scope="module")
def qm(localnet, account):
    return deploy_contract(localnet, account, "QuotaManagerTest")

def test_deploy(qm):
    assert qm.app_id > 0

def test_admin(qm, account):
    r = qm.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_quota(qm):
    boxes = quota_boxes(0)
    qm.send.call(au.AppClientMethodCallParams(
        method="initQuota", args=[0], box_references=boxes))
    r = qm.send.call(au.AppClientMethodCallParams(
        method="createQuota", args=[100],
        box_references=boxes))
    assert r.abi_return == 0

def test_quota_count(qm):
    r = qm.send.call(au.AppClientMethodCallParams(method="getQuotaCount"))
    assert r.abi_return == 1

def test_quota_limit(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getQuotaLimit", args=[0],
        box_references=[au.BoxReference(app_id=0, name=limit_key(0))]))
    assert r.abi_return == 100

def test_quota_used_initial(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getQuotaUsed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=used_key(0))]))
    assert r.abi_return == 0

def test_remaining_initial(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getRemaining", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=limit_key(0)),
            au.BoxReference(app_id=0, name=used_key(0)),
        ]))
    assert r.abi_return == 100

def test_use_quota(qm):
    qm.send.call(au.AppClientMethodCallParams(
        method="useQuota", args=[0, 30],
        box_references=[
            au.BoxReference(app_id=0, name=limit_key(0)),
            au.BoxReference(app_id=0, name=used_key(0)),
        ]))

def test_quota_used_after(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getQuotaUsed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=used_key(0))],
        note=b"u2"))
    assert r.abi_return == 30

def test_remaining_after(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getRemaining", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=limit_key(0)),
            au.BoxReference(app_id=0, name=used_key(0)),
        ], note=b"r2"))
    assert r.abi_return == 70

def test_use_more(qm):
    qm.send.call(au.AppClientMethodCallParams(
        method="useQuota", args=[0, 50],
        box_references=[
            au.BoxReference(app_id=0, name=limit_key(0)),
            au.BoxReference(app_id=0, name=used_key(0)),
        ], note=b"uq2"))

def test_remaining_final(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getRemaining", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=limit_key(0)),
            au.BoxReference(app_id=0, name=used_key(0)),
        ], note=b"r3"))
    assert r.abi_return == 20

def test_create_second(qm):
    boxes = quota_boxes(1)
    qm.send.call(au.AppClientMethodCallParams(
        method="initQuota", args=[1], box_references=boxes))
    r = qm.send.call(au.AppClientMethodCallParams(
        method="createQuota", args=[500],
        box_references=boxes, note=b"q2"))
    assert r.abi_return == 1

def test_count_final(qm):
    r = qm.send.call(au.AppClientMethodCallParams(
        method="getQuotaCount", note=b"qc2"))
    assert r.abi_return == 2
