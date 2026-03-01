"""
Reputation behavioral tests.
Tests entity registration, positive/negative reviews, and net score.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def hash_key(eid):
    return mapping_box_key("_entityHash", eid.to_bytes(64, "big"))
def pos_key(eid):
    return mapping_box_key("_positiveReviews", eid.to_bytes(64, "big"))
def neg_key(eid):
    return mapping_box_key("_negativeReviews", eid.to_bytes(64, "big"))

def entity_boxes(eid):
    return [
        au.BoxReference(app_id=0, name=hash_key(eid)),
        au.BoxReference(app_id=0, name=pos_key(eid)),
        au.BoxReference(app_id=0, name=neg_key(eid)),
    ]

@pytest.fixture(scope="module")
def rep(localnet, account):
    return deploy_contract(localnet, account, "ReputationTest")

def test_deploy(rep):
    assert rep.app_id > 0

def test_admin(rep, account):
    r = rep.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_entity(rep):
    boxes = entity_boxes(0)
    rep.send.call(au.AppClientMethodCallParams(
        method="initEntity", args=[0], box_references=boxes))
    r = rep.send.call(au.AppClientMethodCallParams(
        method="registerEntity", args=[555],
        box_references=boxes))
    assert r.abi_return == 0

def test_entity_count(rep):
    r = rep.send.call(au.AppClientMethodCallParams(method="getEntityCount"))
    assert r.abi_return == 1

def test_entity_hash(rep):
    r = rep.send.call(au.AppClientMethodCallParams(
        method="getEntityHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=hash_key(0))]))
    assert r.abi_return == 555

def test_add_positive_reviews(rep):
    for i in range(3):
        rep.send.call(au.AppClientMethodCallParams(
            method="addPositiveReview", args=[0],
            box_references=[au.BoxReference(app_id=0, name=pos_key(0))],
            note=f"p{i}".encode()))

def test_positive_count(rep):
    r = rep.send.call(au.AppClientMethodCallParams(
        method="getPositiveCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=pos_key(0))]))
    assert r.abi_return == 3

def test_add_negative_review(rep):
    rep.send.call(au.AppClientMethodCallParams(
        method="addNegativeReview", args=[0],
        box_references=[au.BoxReference(app_id=0, name=neg_key(0))]))

def test_negative_count(rep):
    r = rep.send.call(au.AppClientMethodCallParams(
        method="getNegativeCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=neg_key(0))]))
    assert r.abi_return == 1

def test_net_score(rep):
    r = rep.send.call(au.AppClientMethodCallParams(
        method="getNetScore", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=pos_key(0)),
            au.BoxReference(app_id=0, name=neg_key(0)),
        ]))
    assert r.abi_return == 2  # 3 - 1

def test_total_reviews(rep):
    r = rep.send.call(au.AppClientMethodCallParams(method="getTotalReviews"))
    assert r.abi_return == 4  # 3 positive + 1 negative

def test_register_second(rep):
    boxes = entity_boxes(1)
    rep.send.call(au.AppClientMethodCallParams(
        method="initEntity", args=[1], box_references=boxes))
    r = rep.send.call(au.AppClientMethodCallParams(
        method="registerEntity", args=[777],
        box_references=boxes, note=b"r2"))
    assert r.abi_return == 1

def test_count_after(rep):
    r = rep.send.call(au.AppClientMethodCallParams(
        method="getEntityCount", note=b"c2"))
    assert r.abi_return == 2
