"""
PollStation behavioral tests.
Tests poll creation, response submission, averaging, and closing.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def resp_key(pid):
    return mapping_box_key("_pollResponseCount", pid.to_bytes(64, "big"))
def total_key(pid):
    return mapping_box_key("_pollTotalValue", pid.to_bytes(64, "big"))
def open_key(pid):
    return mapping_box_key("_pollOpen", pid.to_bytes(64, "big"))

def poll_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=resp_key(pid)),
        au.BoxReference(app_id=0, name=total_key(pid)),
        au.BoxReference(app_id=0, name=open_key(pid)),
    ]

@pytest.fixture(scope="module")
def ps(localnet, account):
    return deploy_contract(localnet, account, "PollStationTest")

def test_deploy(ps):
    assert ps.app_id > 0

def test_admin(ps, account):
    r = ps.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_poll(ps):
    boxes = poll_boxes(0)
    ps.send.call(au.AppClientMethodCallParams(
        method="initPoll", args=[0], box_references=boxes))
    r = ps.send.call(au.AppClientMethodCallParams(
        method="createPoll", box_references=boxes))
    assert r.abi_return == 0

def test_poll_count(ps):
    r = ps.send.call(au.AppClientMethodCallParams(method="getPollCount"))
    assert r.abi_return == 1

def test_poll_open(ps):
    r = ps.send.call(au.AppClientMethodCallParams(
        method="isPollOpen", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))]))
    assert r.abi_return is True

def test_submit_responses(ps):
    for i, val in enumerate([10, 20, 30]):
        ps.send.call(au.AppClientMethodCallParams(
            method="submitResponse", args=[0, val],
            box_references=[
                au.BoxReference(app_id=0, name=open_key(0)),
                au.BoxReference(app_id=0, name=resp_key(0)),
                au.BoxReference(app_id=0, name=total_key(0)),
            ], note=f"r{i}".encode()))

def test_response_count(ps):
    r = ps.send.call(au.AppClientMethodCallParams(
        method="getPollResponseCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=resp_key(0))]))
    assert r.abi_return == 3

def test_total_value(ps):
    r = ps.send.call(au.AppClientMethodCallParams(
        method="getPollTotalValue", args=[0],
        box_references=[au.BoxReference(app_id=0, name=total_key(0))]))
    assert r.abi_return == 60

def test_average(ps):
    r = ps.send.call(au.AppClientMethodCallParams(
        method="getPollAverage", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=resp_key(0)),
            au.BoxReference(app_id=0, name=total_key(0)),
        ]))
    assert r.abi_return == 20  # 60 / 3

def test_close_poll(ps):
    ps.send.call(au.AppClientMethodCallParams(
        method="closePoll", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))]))

def test_poll_closed(ps):
    r = ps.send.call(au.AppClientMethodCallParams(
        method="isPollOpen", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))],
        note=b"o2"))
    assert r.abi_return is False
