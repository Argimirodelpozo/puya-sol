"""
BadgeSystem behavioral tests.
Tests badge creation, awarding, deactivation/activation, and counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def badge_hash_key(bid):
    return mapping_box_key("_badgeHash", bid.to_bytes(64, "big"))
def awards_key(bid):
    return mapping_box_key("_badgeAwards", bid.to_bytes(64, "big"))
def active_key(bid):
    return mapping_box_key("_badgeActive", bid.to_bytes(64, "big"))

def badge_boxes(bid):
    return [
        au.BoxReference(app_id=0, name=badge_hash_key(bid)),
        au.BoxReference(app_id=0, name=awards_key(bid)),
        au.BoxReference(app_id=0, name=active_key(bid)),
    ]

@pytest.fixture(scope="module")
def bs(localnet, account):
    return deploy_contract(localnet, account, "BadgeSystemTest")

def test_deploy(bs):
    assert bs.app_id > 0

def test_admin(bs, account):
    r = bs.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_badge(bs):
    boxes = badge_boxes(0)
    bs.send.call(au.AppClientMethodCallParams(
        method="initBadge", args=[0], box_references=boxes))
    r = bs.send.call(au.AppClientMethodCallParams(
        method="createBadge", args=[42],
        box_references=boxes))
    assert r.abi_return == 0

def test_badge_count(bs):
    r = bs.send.call(au.AppClientMethodCallParams(method="getBadgeCount"))
    assert r.abi_return == 1

def test_badge_hash(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="getBadgeHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=badge_hash_key(0))]))
    assert r.abi_return == 42

def test_badge_active(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="isBadgeActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_award_badge(bs):
    bs.send.call(au.AppClientMethodCallParams(
        method="awardBadge", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=awards_key(0)),
        ]))

def test_award_count(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="getAwardCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=awards_key(0))]))
    assert r.abi_return == 1

def test_total_awards(bs):
    r = bs.send.call(au.AppClientMethodCallParams(method="getTotalAwards"))
    assert r.abi_return == 1

def test_award_more(bs):
    for i in range(3):
        bs.send.call(au.AppClientMethodCallParams(
            method="awardBadge", args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=awards_key(0)),
            ], note=f"a{i}".encode()))

def test_award_count_after(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="getAwardCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=awards_key(0))],
        note=b"ac2"))
    assert r.abi_return == 4

def test_total_awards_after(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="getTotalAwards", note=b"ta2"))
    assert r.abi_return == 4

def test_deactivate_badge(bs):
    bs.send.call(au.AppClientMethodCallParams(
        method="deactivateBadge", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))

def test_not_active(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="isBadgeActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"ba2"))
    assert r.abi_return is False

def test_reactivate(bs):
    bs.send.call(au.AppClientMethodCallParams(
        method="activateBadge", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))

def test_active_again(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="isBadgeActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"ba3"))
    assert r.abi_return is True

def test_create_second(bs):
    boxes = badge_boxes(1)
    bs.send.call(au.AppClientMethodCallParams(
        method="initBadge", args=[1], box_references=boxes))
    r = bs.send.call(au.AppClientMethodCallParams(
        method="createBadge", args=[99],
        box_references=boxes, note=b"b2"))
    assert r.abi_return == 1

def test_badge_count_final(bs):
    r = bs.send.call(au.AppClientMethodCallParams(
        method="getBadgeCount", note=b"bc2"))
    assert r.abi_return == 2
