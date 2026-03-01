"""
MilestoneTracker behavioral tests.
Tests milestone creation, progress updates, completion, and counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def target_key(mid):
    return mapping_box_key("_milestoneTarget", mid.to_bytes(64, "big"))
def progress_key(mid):
    return mapping_box_key("_milestoneProgress", mid.to_bytes(64, "big"))
def completed_key(mid):
    return mapping_box_key("_milestoneCompleted", mid.to_bytes(64, "big"))

def milestone_boxes(mid):
    return [
        au.BoxReference(app_id=0, name=target_key(mid)),
        au.BoxReference(app_id=0, name=progress_key(mid)),
        au.BoxReference(app_id=0, name=completed_key(mid)),
    ]

@pytest.fixture(scope="module")
def mt(localnet, account):
    return deploy_contract(localnet, account, "MilestoneTrackerTest")

def test_deploy(mt):
    assert mt.app_id > 0

def test_admin(mt, account):
    r = mt.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_milestone(mt):
    boxes = milestone_boxes(0)
    mt.send.call(au.AppClientMethodCallParams(
        method="initMilestone", args=[0], box_references=boxes))
    r = mt.send.call(au.AppClientMethodCallParams(
        method="createMilestone", args=[1000],
        box_references=boxes))
    assert r.abi_return == 0

def test_milestone_count(mt):
    r = mt.send.call(au.AppClientMethodCallParams(method="getMilestoneCount"))
    assert r.abi_return == 1

def test_milestone_target(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="getMilestoneTarget", args=[0],
        box_references=[au.BoxReference(app_id=0, name=target_key(0))]))
    assert r.abi_return == 1000

def test_initial_progress(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="getProgress", args=[0],
        box_references=[au.BoxReference(app_id=0, name=progress_key(0))]))
    assert r.abi_return == 0

def test_not_completed_initially(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="isCompleted", args=[0],
        box_references=[au.BoxReference(app_id=0, name=completed_key(0))]))
    assert r.abi_return is False

def test_update_progress(mt):
    mt.send.call(au.AppClientMethodCallParams(
        method="updateProgress", args=[0, 300],
        box_references=[
            au.BoxReference(app_id=0, name=completed_key(0)),
            au.BoxReference(app_id=0, name=progress_key(0)),
        ]))

def test_progress_after_update(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="getProgress", args=[0],
        box_references=[au.BoxReference(app_id=0, name=progress_key(0))],
        note=b"p2"))
    assert r.abi_return == 300

def test_update_more(mt):
    mt.send.call(au.AppClientMethodCallParams(
        method="updateProgress", args=[0, 500],
        box_references=[
            au.BoxReference(app_id=0, name=completed_key(0)),
            au.BoxReference(app_id=0, name=progress_key(0)),
        ], note=b"u2"))

def test_progress_after_more(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="getProgress", args=[0],
        box_references=[au.BoxReference(app_id=0, name=progress_key(0))],
        note=b"p3"))
    assert r.abi_return == 800

def test_complete_milestone(mt):
    mt.send.call(au.AppClientMethodCallParams(
        method="completeMilestone", args=[0],
        box_references=[au.BoxReference(app_id=0, name=completed_key(0))]))

def test_is_completed(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="isCompleted", args=[0],
        box_references=[au.BoxReference(app_id=0, name=completed_key(0))],
        note=b"c2"))
    assert r.abi_return is True

def test_completed_count(mt):
    r = mt.send.call(au.AppClientMethodCallParams(method="getCompletedCount"))
    assert r.abi_return == 1

def test_create_second(mt):
    boxes = milestone_boxes(1)
    mt.send.call(au.AppClientMethodCallParams(
        method="initMilestone", args=[1], box_references=boxes))
    r = mt.send.call(au.AppClientMethodCallParams(
        method="createMilestone", args=[2000],
        box_references=boxes, note=b"m2"))
    assert r.abi_return == 1

def test_milestone_count_final(mt):
    r = mt.send.call(au.AppClientMethodCallParams(
        method="getMilestoneCount", note=b"mc2"))
    assert r.abi_return == 2
