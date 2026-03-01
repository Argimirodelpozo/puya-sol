"""
RatingSystem behavioral tests.
Tests entity registration, review submission, average scoring.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def entity_hash_key(eid):
    return mapping_box_key("_entityHash", eid.to_bytes(64, "big"))
def total_score_key(eid):
    return mapping_box_key("_totalScore", eid.to_bytes(64, "big"))
def review_count_key(eid):
    return mapping_box_key("_reviewCount", eid.to_bytes(64, "big"))

def entity_boxes(eid):
    return [
        au.BoxReference(app_id=0, name=entity_hash_key(eid)),
        au.BoxReference(app_id=0, name=total_score_key(eid)),
        au.BoxReference(app_id=0, name=review_count_key(eid)),
    ]

@pytest.fixture(scope="module")
def rs(localnet, account):
    return deploy_contract(localnet, account, "RatingSystemTest")

def test_deploy(rs):
    assert rs.app_id > 0

def test_admin(rs, account):
    r = rs.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_entity(rs):
    boxes = entity_boxes(0)
    rs.send.call(au.AppClientMethodCallParams(
        method="initEntity", args=[0], box_references=boxes))
    r = rs.send.call(au.AppClientMethodCallParams(
        method="registerEntity", args=[42],
        box_references=boxes))
    assert r.abi_return == 0

def test_entity_count(rs):
    r = rs.send.call(au.AppClientMethodCallParams(method="getEntityCount"))
    assert r.abi_return == 1

def test_entity_hash(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getEntityHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=entity_hash_key(0))]))
    assert r.abi_return == 42

def test_submit_review(rs):
    rs.send.call(au.AppClientMethodCallParams(
        method="submitReview", args=[0, 80],
        box_references=[
            au.BoxReference(app_id=0, name=total_score_key(0)),
            au.BoxReference(app_id=0, name=review_count_key(0)),
        ]))

def test_review_count(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getReviewCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=review_count_key(0))]))
    assert r.abi_return == 1

def test_global_reviews(rs):
    r = rs.send.call(au.AppClientMethodCallParams(method="getGlobalReviews"))
    assert r.abi_return == 1

def test_average_score(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getAverageScore", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=total_score_key(0)),
            au.BoxReference(app_id=0, name=review_count_key(0)),
        ]))
    assert r.abi_return == 80

def test_submit_more_reviews(rs):
    for i, score in enumerate([60, 100]):
        rs.send.call(au.AppClientMethodCallParams(
            method="submitReview", args=[0, score],
            box_references=[
                au.BoxReference(app_id=0, name=total_score_key(0)),
                au.BoxReference(app_id=0, name=review_count_key(0)),
            ], note=f"r{i}".encode()))

def test_review_count_after(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getReviewCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=review_count_key(0))],
        note=b"rc2"))
    assert r.abi_return == 3

def test_average_after(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getAverageScore", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=total_score_key(0)),
            au.BoxReference(app_id=0, name=review_count_key(0)),
        ], note=b"as2"))
    assert r.abi_return == 80  # (80 + 60 + 100) / 3 = 80

def test_global_reviews_after(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getGlobalReviews", note=b"gr2"))
    assert r.abi_return == 3

def test_register_second(rs):
    boxes = entity_boxes(1)
    rs.send.call(au.AppClientMethodCallParams(
        method="initEntity", args=[1], box_references=boxes))
    r = rs.send.call(au.AppClientMethodCallParams(
        method="registerEntity", args=[99],
        box_references=boxes, note=b"e2"))
    assert r.abi_return == 1

def test_entity_count_final(rs):
    r = rs.send.call(au.AppClientMethodCallParams(
        method="getEntityCount", note=b"ec2"))
    assert r.abi_return == 2
