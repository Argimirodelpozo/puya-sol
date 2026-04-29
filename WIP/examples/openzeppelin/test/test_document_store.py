"""
DocumentStore behavioral tests.
Tests document storage, verification, and revocation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def doc_hash_key(did):
    return mapping_box_key("_docHash", did.to_bytes(64, "big"))
def verified_key(did):
    return mapping_box_key("_docVerified", did.to_bytes(64, "big"))
def revoked_key(did):
    return mapping_box_key("_docRevoked", did.to_bytes(64, "big"))

def doc_boxes(did):
    return [
        au.BoxReference(app_id=0, name=doc_hash_key(did)),
        au.BoxReference(app_id=0, name=verified_key(did)),
        au.BoxReference(app_id=0, name=revoked_key(did)),
    ]

@pytest.fixture(scope="module")
def ds(localnet, account):
    return deploy_contract(localnet, account, "DocumentStoreTest")

def test_deploy(ds):
    assert ds.app_id > 0

def test_admin(ds, account):
    r = ds.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_store_document(ds):
    boxes = doc_boxes(0)
    ds.send.call(au.AppClientMethodCallParams(
        method="initDocument", args=[0], box_references=boxes))
    r = ds.send.call(au.AppClientMethodCallParams(
        method="storeDocument", args=[12345],
        box_references=boxes))
    assert r.abi_return == 0

def test_document_count(ds):
    r = ds.send.call(au.AppClientMethodCallParams(method="getDocumentCount"))
    assert r.abi_return == 1

def test_document_hash(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="getDocumentHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=doc_hash_key(0))]))
    assert r.abi_return == 12345

def test_not_verified_initially(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="isDocumentVerified", args=[0],
        box_references=[au.BoxReference(app_id=0, name=verified_key(0))]))
    assert r.abi_return is False

def test_not_revoked_initially(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="isDocumentRevoked", args=[0],
        box_references=[au.BoxReference(app_id=0, name=revoked_key(0))]))
    assert r.abi_return is False

def test_verify_document(ds):
    ds.send.call(au.AppClientMethodCallParams(
        method="verifyDocument", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=verified_key(0)),
            au.BoxReference(app_id=0, name=revoked_key(0)),
        ]))

def test_is_verified(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="isDocumentVerified", args=[0],
        box_references=[au.BoxReference(app_id=0, name=verified_key(0))],
        note=b"v2"))
    assert r.abi_return is True

def test_verified_count(ds):
    r = ds.send.call(au.AppClientMethodCallParams(method="getVerifiedCount"))
    assert r.abi_return == 1

def test_revoke_document(ds):
    ds.send.call(au.AppClientMethodCallParams(
        method="revokeDocument", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=revoked_key(0)),
            au.BoxReference(app_id=0, name=verified_key(0)),
        ]))

def test_is_revoked(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="isDocumentRevoked", args=[0],
        box_references=[au.BoxReference(app_id=0, name=revoked_key(0))],
        note=b"r2"))
    assert r.abi_return is True

def test_verified_count_after_revoke(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="getVerifiedCount", note=b"vc2"))
    assert r.abi_return == 0

def test_store_second(ds):
    boxes = doc_boxes(1)
    ds.send.call(au.AppClientMethodCallParams(
        method="initDocument", args=[1], box_references=boxes))
    r = ds.send.call(au.AppClientMethodCallParams(
        method="storeDocument", args=[67890],
        box_references=boxes, note=b"s2"))
    assert r.abi_return == 1

def test_count_after(ds):
    r = ds.send.call(au.AppClientMethodCallParams(
        method="getDocumentCount", note=b"c2"))
    assert r.abi_return == 2
