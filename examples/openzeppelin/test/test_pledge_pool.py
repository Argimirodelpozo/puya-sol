"""
PledgePool behavioral tests.
Tests pledge creation, contributions, funding status.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def amount_key(pid):
    return mapping_box_key("_pledgeAmount", pid.to_bytes(64, "big"))
def goal_key(pid):
    return mapping_box_key("_pledgeGoal", pid.to_bytes(64, "big"))
def funded_key(pid):
    return mapping_box_key("_pledgeFunded", pid.to_bytes(64, "big"))

def pledge_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=amount_key(pid)),
        au.BoxReference(app_id=0, name=goal_key(pid)),
        au.BoxReference(app_id=0, name=funded_key(pid)),
    ]

@pytest.fixture(scope="module")
def pp(localnet, account):
    return deploy_contract(localnet, account, "PledgePoolTest")

def test_deploy(pp):
    assert pp.app_id > 0

def test_admin(pp, account):
    r = pp.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_pledge(pp):
    boxes = pledge_boxes(0)
    pp.send.call(au.AppClientMethodCallParams(
        method="initPledge", args=[0], box_references=boxes))
    r = pp.send.call(au.AppClientMethodCallParams(
        method="createPledge", args=[1000],
        box_references=boxes))
    assert r.abi_return == 0

def test_pledge_count(pp):
    r = pp.send.call(au.AppClientMethodCallParams(method="getPledgeCount"))
    assert r.abi_return == 1

def test_pledge_goal(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getPledgeGoal", args=[0],
        box_references=[au.BoxReference(app_id=0, name=goal_key(0))]))
    assert r.abi_return == 1000

def test_pledge_amount_initial(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getPledgeAmount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=amount_key(0))]))
    assert r.abi_return == 0

def test_not_funded_initially(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="isPledgeFunded", args=[0],
        box_references=[au.BoxReference(app_id=0, name=funded_key(0))]))
    assert r.abi_return is False

def test_contribute(pp):
    pp.send.call(au.AppClientMethodCallParams(
        method="contribute", args=[0, 400],
        box_references=[au.BoxReference(app_id=0, name=amount_key(0))]))

def test_amount_after_contribute(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getPledgeAmount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
        note=b"a2"))
    assert r.abi_return == 400

def test_total_pledged(pp):
    r = pp.send.call(au.AppClientMethodCallParams(method="getTotalPledged"))
    assert r.abi_return == 400

def test_contribute_more(pp):
    for i, amt in enumerate([300, 300]):
        pp.send.call(au.AppClientMethodCallParams(
            method="contribute", args=[0, amt],
            box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
            note=f"c{i}".encode()))

def test_amount_at_goal(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getPledgeAmount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
        note=b"a3"))
    assert r.abi_return == 1000

def test_mark_funded(pp):
    pp.send.call(au.AppClientMethodCallParams(
        method="markFunded", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=amount_key(0)),
            au.BoxReference(app_id=0, name=goal_key(0)),
            au.BoxReference(app_id=0, name=funded_key(0)),
        ]))

def test_is_funded(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="isPledgeFunded", args=[0],
        box_references=[au.BoxReference(app_id=0, name=funded_key(0))],
        note=b"f2"))
    assert r.abi_return is True

def test_total_pledged_final(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getTotalPledged", note=b"tp2"))
    assert r.abi_return == 1000

def test_create_second(pp):
    boxes = pledge_boxes(1)
    pp.send.call(au.AppClientMethodCallParams(
        method="initPledge", args=[1], box_references=boxes))
    r = pp.send.call(au.AppClientMethodCallParams(
        method="createPledge", args=[500],
        box_references=boxes, note=b"p2"))
    assert r.abi_return == 1

def test_count_final(pp):
    r = pp.send.call(au.AppClientMethodCallParams(
        method="getPledgeCount", note=b"pc2"))
    assert r.abi_return == 2
