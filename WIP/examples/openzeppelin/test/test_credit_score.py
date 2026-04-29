"""
CreditScore behavioral tests.
Tests borrower registration, score adjustment, payments, and deactivation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def score_key(bid):
    return mapping_box_key("_borrowerScore", bid.to_bytes(64, "big"))
def payment_key(bid):
    return mapping_box_key("_paymentCount", bid.to_bytes(64, "big"))
def active_key(bid):
    return mapping_box_key("_borrowerActive", bid.to_bytes(64, "big"))

def borrower_boxes(bid):
    return [
        au.BoxReference(app_id=0, name=score_key(bid)),
        au.BoxReference(app_id=0, name=payment_key(bid)),
        au.BoxReference(app_id=0, name=active_key(bid)),
    ]

@pytest.fixture(scope="module")
def cs(localnet, account):
    return deploy_contract(localnet, account, "CreditScoreTest")

def test_deploy(cs):
    assert cs.app_id > 0

def test_admin(cs, account):
    r = cs.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_borrower(cs):
    boxes = borrower_boxes(0)
    cs.send.call(au.AppClientMethodCallParams(
        method="initBorrower", args=[0], box_references=boxes))
    r = cs.send.call(au.AppClientMethodCallParams(
        method="registerBorrower", args=[700],
        box_references=boxes))
    assert r.abi_return == 0

def test_borrower_count(cs):
    r = cs.send.call(au.AppClientMethodCallParams(method="getBorrowerCount"))
    assert r.abi_return == 1

def test_borrower_score(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="getBorrowerScore", args=[0],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))]))
    assert r.abi_return == 700

def test_borrower_active(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="isBorrowerActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_adjust_score(cs):
    cs.send.call(au.AppClientMethodCallParams(
        method="adjustScore", args=[0, 750],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))]))

def test_score_after_adjust(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="getBorrowerScore", args=[0],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))],
        note=b"s2"))
    assert r.abi_return == 750

def test_record_payment(cs):
    cs.send.call(au.AppClientMethodCallParams(
        method="recordPayment", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=payment_key(0)),
        ]))

def test_payment_count(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="getPaymentCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=payment_key(0))]))
    assert r.abi_return == 1

def test_record_more_payments(cs):
    for i in range(2):
        cs.send.call(au.AppClientMethodCallParams(
            method="recordPayment", args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=payment_key(0)),
            ], note=f"p{i}".encode()))

def test_payment_count_after(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="getPaymentCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=payment_key(0))],
        note=b"pc2"))
    assert r.abi_return == 3

def test_deactivate(cs):
    cs.send.call(au.AppClientMethodCallParams(
        method="deactivateBorrower", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))

def test_not_active(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="isBorrowerActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is False

def test_register_second(cs):
    boxes = borrower_boxes(1)
    cs.send.call(au.AppClientMethodCallParams(
        method="initBorrower", args=[1], box_references=boxes))
    cs.send.call(au.AppClientMethodCallParams(
        method="registerBorrower", args=[650],
        box_references=boxes, note=b"r2"))

def test_count_after(cs):
    r = cs.send.call(au.AppClientMethodCallParams(
        method="getBorrowerCount", note=b"c2"))
    assert r.abi_return == 2
