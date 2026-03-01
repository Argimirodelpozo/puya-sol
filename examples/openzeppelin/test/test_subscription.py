"""
Subscription behavioral tests.
Tests subscribing, cancelling, and querying subscription state.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def plan_key(sid):
    return mapping_box_key("_subPlan", sid.to_bytes(64, "big"))
def active_key(sid):
    return mapping_box_key("_subActive", sid.to_bytes(64, "big"))
def owner_key(sid):
    return mapping_box_key("_subOwner", sid.to_bytes(64, "big"))

def sub_boxes(sid):
    return [
        au.BoxReference(app_id=0, name=plan_key(sid)),
        au.BoxReference(app_id=0, name=active_key(sid)),
        au.BoxReference(app_id=0, name=owner_key(sid)),
    ]

@pytest.fixture(scope="module")
def subs(localnet, account):
    return deploy_contract(localnet, account, "SubscriptionTest")

def test_deploy(subs):
    assert subs.app_id > 0

def test_admin(subs, account):
    r = subs.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_subscribe(subs):
    boxes = sub_boxes(0)
    subs.send.call(au.AppClientMethodCallParams(
        method="initSubscription", args=[0], box_references=boxes))
    r = subs.send.call(au.AppClientMethodCallParams(
        method="subscribe", args=[10],
        box_references=boxes))
    assert r.abi_return == 0

def test_subscription_count(subs):
    r = subs.send.call(au.AppClientMethodCallParams(method="getSubscriptionCount"))
    assert r.abi_return == 1

def test_active_count(subs):
    r = subs.send.call(au.AppClientMethodCallParams(method="getActiveCount"))
    assert r.abi_return == 1

def test_subscription_plan(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="getSubscriptionPlan", args=[0],
        box_references=[au.BoxReference(app_id=0, name=plan_key(0))]))
    assert r.abi_return == 10

def test_subscription_active(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="isSubscriptionActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_subscription_owner(subs, account):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="getSubscriptionOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    assert r.abi_return == account.address

def test_cancel_subscription(subs):
    subs.send.call(au.AppClientMethodCallParams(
        method="cancelSubscription", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=owner_key(0)),
        ]))

def test_not_active_after_cancel(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="isSubscriptionActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is False

def test_active_count_after_cancel(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="getActiveCount", note=b"ac2"))
    assert r.abi_return == 0

def test_subscribe_second(subs):
    boxes = sub_boxes(1)
    subs.send.call(au.AppClientMethodCallParams(
        method="initSubscription", args=[1], box_references=boxes))
    r = subs.send.call(au.AppClientMethodCallParams(
        method="subscribe", args=[20],
        box_references=boxes, note=b"s2"))
    assert r.abi_return == 1

def test_count_after_second(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="getSubscriptionCount", note=b"c2"))
    assert r.abi_return == 2

def test_active_count_final(subs):
    r = subs.send.call(au.AppClientMethodCallParams(
        method="getActiveCount", note=b"ac3"))
    assert r.abi_return == 1
