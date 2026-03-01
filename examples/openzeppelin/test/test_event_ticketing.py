"""
EventTicketing behavioral tests.
Tests event creation, ticket buying, usage, and capacity tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def ticket_event_key(tid):
    return mapping_box_key("_ticketEvent", tid.to_bytes(64, "big"))
def ticket_used_key(tid):
    return mapping_box_key("_ticketUsed", tid.to_bytes(64, "big"))
def event_cap_key(eid):
    return mapping_box_key("_eventCapacity", eid.to_bytes(64, "big"))

def ticket_boxes(tid):
    return [
        au.BoxReference(app_id=0, name=ticket_event_key(tid)),
        au.BoxReference(app_id=0, name=ticket_used_key(tid)),
    ]

@pytest.fixture(scope="module")
def et(localnet, account):
    return deploy_contract(localnet, account, "EventTicketingTest")

def test_deploy(et):
    assert et.app_id > 0

def test_admin(et, account):
    r = et.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_event(et):
    et.send.call(au.AppClientMethodCallParams(
        method="initEvent", args=[0],
        box_references=[au.BoxReference(app_id=0, name=event_cap_key(0))]))
    r = et.send.call(au.AppClientMethodCallParams(
        method="createEvent", args=[10],
        box_references=[au.BoxReference(app_id=0, name=event_cap_key(0))]))
    assert r.abi_return == 0

def test_event_count(et):
    r = et.send.call(au.AppClientMethodCallParams(method="getEventCount"))
    assert r.abi_return == 1

def test_event_capacity(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="getEventCapacity", args=[0],
        box_references=[au.BoxReference(app_id=0, name=event_cap_key(0))]))
    assert r.abi_return == 10

def test_buy_ticket(et):
    boxes = ticket_boxes(0) + [au.BoxReference(app_id=0, name=event_cap_key(0))]
    et.send.call(au.AppClientMethodCallParams(
        method="initTicket", args=[0],
        box_references=ticket_boxes(0)))
    r = et.send.call(au.AppClientMethodCallParams(
        method="buyTicket", args=[0],
        box_references=boxes))
    assert r.abi_return == 0

def test_ticket_count(et):
    r = et.send.call(au.AppClientMethodCallParams(method="getTicketCount"))
    assert r.abi_return == 1

def test_ticket_event(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="getTicketEvent", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ticket_event_key(0))]))
    assert r.abi_return == 0

def test_ticket_not_used(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="isTicketUsed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ticket_used_key(0))]))
    assert r.abi_return is False

def test_capacity_after_buy(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="getEventCapacity", args=[0],
        box_references=[au.BoxReference(app_id=0, name=event_cap_key(0))],
        note=b"c2"))
    assert r.abi_return == 9

def test_use_ticket(et):
    et.send.call(au.AppClientMethodCallParams(
        method="useTicket", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ticket_used_key(0))]))

def test_ticket_used(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="isTicketUsed", args=[0],
        box_references=[au.BoxReference(app_id=0, name=ticket_used_key(0))],
        note=b"u2"))
    assert r.abi_return is True

def test_buy_second_ticket(et):
    boxes = ticket_boxes(1) + [au.BoxReference(app_id=0, name=event_cap_key(0))]
    et.send.call(au.AppClientMethodCallParams(
        method="initTicket", args=[1],
        box_references=ticket_boxes(1)))
    r = et.send.call(au.AppClientMethodCallParams(
        method="buyTicket", args=[0],
        box_references=boxes, note=b"b2"))
    assert r.abi_return == 1

def test_capacity_final(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="getEventCapacity", args=[0],
        box_references=[au.BoxReference(app_id=0, name=event_cap_key(0))],
        note=b"c3"))
    assert r.abi_return == 8

def test_ticket_count_final(et):
    r = et.send.call(au.AppClientMethodCallParams(
        method="getTicketCount", note=b"tc2"))
    assert r.abi_return == 2
