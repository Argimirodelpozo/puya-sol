"""
CertificateIssuer behavioral tests.
Tests certificate issuance, verification, revocation, and counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def hash_key(cid):
    return mapping_box_key("_certHash", cid.to_bytes(64, "big"))
def issued_key(cid):
    return mapping_box_key("_certIssued", cid.to_bytes(64, "big"))
def revoked_key(cid):
    return mapping_box_key("_certRevoked", cid.to_bytes(64, "big"))

def cert_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=hash_key(cid)),
        au.BoxReference(app_id=0, name=issued_key(cid)),
        au.BoxReference(app_id=0, name=revoked_key(cid)),
    ]

@pytest.fixture(scope="module")
def ci(localnet, account):
    return deploy_contract(localnet, account, "CertificateIssuerTest")

def test_deploy(ci):
    assert ci.app_id > 0

def test_admin(ci, account):
    r = ci.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_issue_certificate(ci):
    boxes = cert_boxes(0)
    ci.send.call(au.AppClientMethodCallParams(
        method="initCert", args=[0], box_references=boxes))
    r = ci.send.call(au.AppClientMethodCallParams(
        method="issueCertificate", args=[12345],
        box_references=boxes))
    assert r.abi_return == 0

def test_cert_count(ci):
    r = ci.send.call(au.AppClientMethodCallParams(method="getCertCount"))
    assert r.abi_return == 1

def test_cert_hash(ci):
    r = ci.send.call(au.AppClientMethodCallParams(
        method="getCertHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=hash_key(0))]))
    assert r.abi_return == 12345

def test_verify_certificate(ci):
    r = ci.send.call(au.AppClientMethodCallParams(
        method="verifyCertificate", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=issued_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ]))
    assert r.abi_return is True

def test_revoke_certificate(ci):
    ci.send.call(au.AppClientMethodCallParams(
        method="revokeCertificate", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=issued_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ]))

def test_revoked_count(ci):
    r = ci.send.call(au.AppClientMethodCallParams(method="getRevokedCount"))
    assert r.abi_return == 1

def test_verify_after_revoke(ci):
    r = ci.send.call(au.AppClientMethodCallParams(
        method="verifyCertificate", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=issued_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ], note=b"v2"))
    assert r.abi_return is False

def test_issue_second(ci):
    boxes = cert_boxes(1)
    ci.send.call(au.AppClientMethodCallParams(
        method="initCert", args=[1], box_references=boxes))
    r = ci.send.call(au.AppClientMethodCallParams(
        method="issueCertificate", args=[67890],
        box_references=boxes, note=b"i2"))
    assert r.abi_return == 1

def test_cert_count_after(ci):
    r = ci.send.call(au.AppClientMethodCallParams(
        method="getCertCount", note=b"c2"))
    assert r.abi_return == 2

def test_verify_second(ci):
    r = ci.send.call(au.AppClientMethodCallParams(
        method="verifyCertificate", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=issued_key(1)),
            au.BoxReference(app_id=0, name=revoked_key(1)),
        ]))
    assert r.abi_return is True
