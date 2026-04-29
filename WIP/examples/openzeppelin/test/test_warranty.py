"""
Warranty behavioral tests.
Tests product registration, warranty claims, and voiding.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def expiry_key(pid):
    return mapping_box_key("_productExpiry", pid.to_bytes(64, "big"))
def voided_key(pid):
    return mapping_box_key("_productVoided", pid.to_bytes(64, "big"))
def claims_key(pid):
    return mapping_box_key("_productClaims", pid.to_bytes(64, "big"))

def product_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=expiry_key(pid)),
        au.BoxReference(app_id=0, name=voided_key(pid)),
        au.BoxReference(app_id=0, name=claims_key(pid)),
    ]

@pytest.fixture(scope="module")
def war(localnet, account):
    return deploy_contract(localnet, account, "WarrantyTest")

def test_deploy(war):
    assert war.app_id > 0

def test_admin(war, account):
    r = war.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_product(war):
    boxes = product_boxes(0)
    war.send.call(au.AppClientMethodCallParams(
        method="initProduct", args=[0], box_references=boxes))
    r = war.send.call(au.AppClientMethodCallParams(
        method="registerProduct", args=[999999],
        box_references=boxes))
    assert r.abi_return == 0

def test_product_count(war):
    r = war.send.call(au.AppClientMethodCallParams(method="getProductCount"))
    assert r.abi_return == 1

def test_expiry_time(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="getExpiryTime", args=[0],
        box_references=[au.BoxReference(app_id=0, name=expiry_key(0))]))
    assert r.abi_return == 999999

def test_warranty_valid(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="isWarrantyValid", args=[0],
        box_references=[au.BoxReference(app_id=0, name=voided_key(0))]))
    assert r.abi_return is True

def test_claim_warranty(war):
    war.send.call(au.AppClientMethodCallParams(
        method="claimWarranty", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=voided_key(0)),
            au.BoxReference(app_id=0, name=claims_key(0)),
        ]))

def test_claim_count(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="getClaimCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=claims_key(0))]))
    assert r.abi_return == 1

def test_claim_again(war):
    war.send.call(au.AppClientMethodCallParams(
        method="claimWarranty", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=voided_key(0)),
            au.BoxReference(app_id=0, name=claims_key(0)),
        ], note=b"c2"))

def test_claim_count_after(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="getClaimCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=claims_key(0))],
        note=b"cc2"))
    assert r.abi_return == 2

def test_void_warranty(war):
    war.send.call(au.AppClientMethodCallParams(
        method="voidWarranty", args=[0],
        box_references=[au.BoxReference(app_id=0, name=voided_key(0))]))

def test_warranty_voided(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="isWarrantyVoided", args=[0],
        box_references=[au.BoxReference(app_id=0, name=voided_key(0))]))
    assert r.abi_return is True

def test_warranty_not_valid(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="isWarrantyValid", args=[0],
        box_references=[au.BoxReference(app_id=0, name=voided_key(0))],
        note=b"v2"))
    assert r.abi_return is False

def test_register_second(war):
    boxes = product_boxes(1)
    war.send.call(au.AppClientMethodCallParams(
        method="initProduct", args=[1], box_references=boxes))
    r = war.send.call(au.AppClientMethodCallParams(
        method="registerProduct", args=[888888],
        box_references=boxes, note=b"r2"))
    assert r.abi_return == 1

def test_count_after(war):
    r = war.send.call(au.AppClientMethodCallParams(
        method="getProductCount", note=b"p2"))
    assert r.abi_return == 2
