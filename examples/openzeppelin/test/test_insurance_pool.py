"""
InsurancePool behavioral tests.
Tests policy creation, activation, claims, and premium tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def premium_key(pid):
    return mapping_box_key("_policyPremium", pid.to_bytes(64, "big"))
def coverage_key(pid):
    return mapping_box_key("_policyCoverage", pid.to_bytes(64, "big"))
def active_key(pid):
    return mapping_box_key("_policyActive", pid.to_bytes(64, "big"))
def claims_key(pid):
    return mapping_box_key("_policyClaims", pid.to_bytes(64, "big"))

def policy_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=premium_key(pid)),
        au.BoxReference(app_id=0, name=coverage_key(pid)),
        au.BoxReference(app_id=0, name=active_key(pid)),
        au.BoxReference(app_id=0, name=claims_key(pid)),
    ]

@pytest.fixture(scope="module")
def ins(localnet, account):
    return deploy_contract(localnet, account, "InsurancePoolTest")

def test_deploy(ins):
    assert ins.app_id > 0

def test_admin(ins, account):
    r = ins.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_policy(ins):
    boxes = policy_boxes(0)
    ins.send.call(au.AppClientMethodCallParams(
        method="initPolicy", args=[0], box_references=boxes))
    r = ins.send.call(au.AppClientMethodCallParams(
        method="createPolicy", args=[100, 5000],
        box_references=boxes))
    assert r.abi_return == 0

def test_policy_count(ins):
    r = ins.send.call(au.AppClientMethodCallParams(method="getPolicyCount"))
    assert r.abi_return == 1

def test_premium(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="getPremium", args=[0],
        box_references=[au.BoxReference(app_id=0, name=premium_key(0))]))
    assert r.abi_return == 100

def test_coverage(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="getCoverage", args=[0],
        box_references=[au.BoxReference(app_id=0, name=coverage_key(0))]))
    assert r.abi_return == 5000

def test_not_active_initially(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="isPolicyActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is False

def test_activate_policy(ins):
    ins.send.call(au.AppClientMethodCallParams(
        method="activatePolicy", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))

def test_is_active(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="isPolicyActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is True

def test_file_claim(ins):
    ins.send.call(au.AppClientMethodCallParams(
        method="fileClaim", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=claims_key(0)),
        ]))

def test_claim_count(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="getClaimCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=claims_key(0))]))
    assert r.abi_return == 1

def test_file_second_claim(ins):
    ins.send.call(au.AppClientMethodCallParams(
        method="fileClaim", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=claims_key(0)),
        ], note=b"c2"))

def test_claim_count_after(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="getClaimCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=claims_key(0))],
        note=b"cc2"))
    assert r.abi_return == 2

def test_total_premiums(ins):
    r = ins.send.call(au.AppClientMethodCallParams(method="getTotalPremiums"))
    assert r.abi_return == 100

def test_create_second_policy(ins):
    boxes = policy_boxes(1)
    ins.send.call(au.AppClientMethodCallParams(
        method="initPolicy", args=[1], box_references=boxes))
    r = ins.send.call(au.AppClientMethodCallParams(
        method="createPolicy", args=[200, 10000],
        box_references=boxes, note=b"p2"))
    assert r.abi_return == 1

def test_activate_second(ins):
    ins.send.call(au.AppClientMethodCallParams(
        method="activatePolicy", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(1)),
            au.BoxReference(app_id=0, name=premium_key(1)),
        ]))

def test_total_premiums_after(ins):
    r = ins.send.call(au.AppClientMethodCallParams(
        method="getTotalPremiums", note=b"tp2"))
    assert r.abi_return == 300
