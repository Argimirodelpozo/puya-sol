"""
LicenseManager behavioral tests.
Tests license issuance, activation, revocation, and counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def prod_key(lid):
    return mapping_box_key("_licenseProduct", lid.to_bytes(64, "big"))
def active_key(lid):
    return mapping_box_key("_licenseActive", lid.to_bytes(64, "big"))
def revoked_key(lid):
    return mapping_box_key("_licenseRevoked", lid.to_bytes(64, "big"))

def license_boxes(lid):
    return [
        au.BoxReference(app_id=0, name=prod_key(lid)),
        au.BoxReference(app_id=0, name=active_key(lid)),
        au.BoxReference(app_id=0, name=revoked_key(lid)),
    ]

@pytest.fixture(scope="module")
def lm(localnet, account):
    return deploy_contract(localnet, account, "LicenseManagerTest")

def test_deploy(lm):
    assert lm.app_id > 0

def test_admin(lm, account):
    r = lm.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_issue_license(lm):
    boxes = license_boxes(0)
    lm.send.call(au.AppClientMethodCallParams(
        method="initLicense", args=[0], box_references=boxes))
    r = lm.send.call(au.AppClientMethodCallParams(
        method="issueLicense", args=[500],
        box_references=boxes))
    assert r.abi_return == 0

def test_license_count(lm):
    r = lm.send.call(au.AppClientMethodCallParams(method="getLicenseCount"))
    assert r.abi_return == 1

def test_product_hash(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="getProductHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=prod_key(0))]))
    assert r.abi_return == 500

def test_license_not_active_initially(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="isLicenseActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is False

def test_activate_license(lm):
    lm.send.call(au.AppClientMethodCallParams(
        method="activateLicense", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ]))

def test_license_active(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="isLicenseActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is True

def test_active_count(lm):
    r = lm.send.call(au.AppClientMethodCallParams(method="getActiveLicenseCount"))
    assert r.abi_return == 1

def test_revoke_license(lm):
    lm.send.call(au.AppClientMethodCallParams(
        method="revokeLicense", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ]))

def test_not_active_after_revoke(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="isLicenseActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a3"))
    assert r.abi_return is False

def test_is_revoked(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="isLicenseRevoked", args=[0],
        box_references=[au.BoxReference(app_id=0, name=revoked_key(0))]))
    assert r.abi_return is True

def test_active_count_after_revoke(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="getActiveLicenseCount", note=b"ac2"))
    assert r.abi_return == 0

def test_issue_second(lm):
    boxes = license_boxes(1)
    lm.send.call(au.AppClientMethodCallParams(
        method="initLicense", args=[1], box_references=boxes))
    r = lm.send.call(au.AppClientMethodCallParams(
        method="issueLicense", args=[600],
        box_references=boxes, note=b"i2"))
    assert r.abi_return == 1

def test_activate_second(lm):
    lm.send.call(au.AppClientMethodCallParams(
        method="activateLicense", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(1)),
            au.BoxReference(app_id=0, name=revoked_key(1)),
        ]))

def test_active_count_final(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="getActiveLicenseCount", note=b"ac3"))
    assert r.abi_return == 1

def test_count_final(lm):
    r = lm.send.call(au.AppClientMethodCallParams(
        method="getLicenseCount", note=b"c2"))
    assert r.abi_return == 2
