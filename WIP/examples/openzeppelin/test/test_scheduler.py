"""
Scheduler behavioral tests.
Tests event scheduling, cancellation, completion, and status queries.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def timestamp_key(eid):
    return mapping_box_key("_eventTimestamp", eid.to_bytes(64, "big"))
def cancelled_key(eid):
    return mapping_box_key("_eventCancelled", eid.to_bytes(64, "big"))
def completed_key(eid):
    return mapping_box_key("_eventCompleted", eid.to_bytes(64, "big"))

def event_boxes(eid):
    return [
        au.BoxReference(app_id=0, name=timestamp_key(eid)),
        au.BoxReference(app_id=0, name=cancelled_key(eid)),
        au.BoxReference(app_id=0, name=completed_key(eid)),
    ]

@pytest.fixture(scope="module")
def sch(localnet, account):
    return deploy_contract(localnet, account, "SchedulerTest")

def test_deploy(sch):
    assert sch.app_id > 0

def test_admin(sch, account):
    r = sch.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_schedule_event(sch):
    boxes = event_boxes(0)
    sch.send.call(au.AppClientMethodCallParams(
        method="initEvent", args=[0], box_references=boxes))
    r = sch.send.call(au.AppClientMethodCallParams(
        method="scheduleEvent", args=[5000],
        box_references=boxes))
    assert r.abi_return == 0

def test_event_count(sch):
    r = sch.send.call(au.AppClientMethodCallParams(method="getEventCount"))
    assert r.abi_return == 1

def test_event_timestamp(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="getEventTimestamp", args=[0],
        box_references=[au.BoxReference(app_id=0, name=timestamp_key(0))]))
    assert r.abi_return == 5000

def test_is_scheduled(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="isEventScheduled", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=cancelled_key(0)),
            au.BoxReference(app_id=0, name=completed_key(0)),
        ]))
    assert r.abi_return is True

def test_complete_event(sch):
    sch.send.call(au.AppClientMethodCallParams(
        method="completeEvent", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=cancelled_key(0)),
            au.BoxReference(app_id=0, name=completed_key(0)),
        ]))

def test_is_completed(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="isEventCompleted", args=[0],
        box_references=[au.BoxReference(app_id=0, name=completed_key(0))]))
    assert r.abi_return is True

def test_not_scheduled_after_complete(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="isEventScheduled", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=cancelled_key(0)),
            au.BoxReference(app_id=0, name=completed_key(0)),
        ], note=b"s2"))
    assert r.abi_return is False

def test_completed_count(sch):
    r = sch.send.call(au.AppClientMethodCallParams(method="getCompletedCount"))
    assert r.abi_return == 1

def test_schedule_and_cancel(sch):
    boxes = event_boxes(1)
    sch.send.call(au.AppClientMethodCallParams(
        method="initEvent", args=[1], box_references=boxes))
    sch.send.call(au.AppClientMethodCallParams(
        method="scheduleEvent", args=[6000],
        box_references=boxes, note=b"e2"))
    sch.send.call(au.AppClientMethodCallParams(
        method="cancelEvent", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=cancelled_key(1)),
            au.BoxReference(app_id=0, name=completed_key(1)),
        ]))

def test_cancelled_not_scheduled(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="isEventScheduled", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=cancelled_key(1)),
            au.BoxReference(app_id=0, name=completed_key(1)),
        ]))
    assert r.abi_return is False

def test_event_count_final(sch):
    r = sch.send.call(au.AppClientMethodCallParams(
        method="getEventCount", note=b"c2"))
    assert r.abi_return == 2
