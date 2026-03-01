"""
ContentRegistry behavioral tests.
Tests content publishing, rating, average calculation, and queries.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def hash_key(cid):
    return mapping_box_key("_contentHash", cid.to_bytes(64, "big"))
def rating_key(cid):
    return mapping_box_key("_totalRating", cid.to_bytes(64, "big"))
def rater_key(cid):
    return mapping_box_key("_raterCount", cid.to_bytes(64, "big"))
def pub_key(cid):
    return mapping_box_key("_contentPublished", cid.to_bytes(64, "big"))

def content_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=hash_key(cid)),
        au.BoxReference(app_id=0, name=rating_key(cid)),
        au.BoxReference(app_id=0, name=rater_key(cid)),
        au.BoxReference(app_id=0, name=pub_key(cid)),
    ]

@pytest.fixture(scope="module")
def cr(localnet, account):
    return deploy_contract(localnet, account, "ContentRegistryTest")

def test_deploy(cr):
    assert cr.app_id > 0

def test_admin(cr, account):
    r = cr.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_publish_content(cr):
    boxes = content_boxes(0)
    cr.send.call(au.AppClientMethodCallParams(
        method="initContent", args=[0], box_references=boxes))
    r = cr.send.call(au.AppClientMethodCallParams(
        method="publishContent", args=[42],
        box_references=boxes))
    assert r.abi_return == 0

def test_content_count(cr):
    r = cr.send.call(au.AppClientMethodCallParams(method="getContentCount"))
    assert r.abi_return == 1

def test_content_hash(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getContentHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=hash_key(0))]))
    assert r.abi_return == 42

def test_content_published(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="isContentPublished", args=[0],
        box_references=[au.BoxReference(app_id=0, name=pub_key(0))]))
    assert r.abi_return is True

def test_rate_content(cr):
    cr.send.call(au.AppClientMethodCallParams(
        method="rateContent", args=[0, 80],
        box_references=[
            au.BoxReference(app_id=0, name=pub_key(0)),
            au.BoxReference(app_id=0, name=rating_key(0)),
            au.BoxReference(app_id=0, name=rater_key(0)),
        ]))

def test_rater_count(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getRaterCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rater_key(0))]))
    assert r.abi_return == 1

def test_total_rating(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getTotalRating", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rating_key(0))]))
    assert r.abi_return == 80

def test_rate_more(cr):
    for i, val in enumerate([60, 100]):
        cr.send.call(au.AppClientMethodCallParams(
            method="rateContent", args=[0, val],
            box_references=[
                au.BoxReference(app_id=0, name=pub_key(0)),
                au.BoxReference(app_id=0, name=rating_key(0)),
                au.BoxReference(app_id=0, name=rater_key(0)),
            ], note=f"r{i}".encode()))

def test_rater_count_after(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getRaterCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rater_key(0))],
        note=b"rc2"))
    assert r.abi_return == 3

def test_average_rating(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getAverageRating", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=rating_key(0)),
            au.BoxReference(app_id=0, name=rater_key(0)),
        ]))
    assert r.abi_return == 80  # (80 + 60 + 100) / 3 = 80

def test_publish_second(cr):
    boxes = content_boxes(1)
    cr.send.call(au.AppClientMethodCallParams(
        method="initContent", args=[1], box_references=boxes))
    r = cr.send.call(au.AppClientMethodCallParams(
        method="publishContent", args=[99],
        box_references=boxes, note=b"p2"))
    assert r.abi_return == 1

def test_count_after(cr):
    r = cr.send.call(au.AppClientMethodCallParams(
        method="getContentCount", note=b"c2"))
    assert r.abi_return == 2
