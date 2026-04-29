"""
WhitelistManager behavioral tests.
Tests whitelist add/remove, batch operations, and count tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def wl_key(addr_hash):
    return mapping_box_key("_whitelisted", addr_hash.to_bytes(64, "big"))

@pytest.fixture(scope="module")
def wl(localnet, account):
    return deploy_contract(localnet, account, "WhitelistManagerTest")

def test_deploy(wl):
    assert wl.app_id > 0

def test_admin(wl, account):
    r = wl.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_init_and_add(wl):
    wl.send.call(au.AppClientMethodCallParams(
        method="initWhitelistEntry", args=[100],
        box_references=[au.BoxReference(app_id=0, name=wl_key(100))]))
    wl.send.call(au.AppClientMethodCallParams(
        method="addToWhitelist", args=[100],
        box_references=[au.BoxReference(app_id=0, name=wl_key(100))]))

def test_is_whitelisted(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="isWhitelisted", args=[100],
        box_references=[au.BoxReference(app_id=0, name=wl_key(100))]))
    assert r.abi_return is True

def test_whitelist_count(wl):
    r = wl.send.call(au.AppClientMethodCallParams(method="getWhitelistCount"))
    assert r.abi_return == 1

def test_add_second(wl):
    wl.send.call(au.AppClientMethodCallParams(
        method="initWhitelistEntry", args=[200],
        box_references=[au.BoxReference(app_id=0, name=wl_key(200))]))
    wl.send.call(au.AppClientMethodCallParams(
        method="addToWhitelist", args=[200],
        box_references=[au.BoxReference(app_id=0, name=wl_key(200))]))

def test_count_after_second(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="getWhitelistCount", note=b"c2"))
    assert r.abi_return == 2

def test_remove(wl):
    wl.send.call(au.AppClientMethodCallParams(
        method="removeFromWhitelist", args=[100],
        box_references=[au.BoxReference(app_id=0, name=wl_key(100))]))

def test_not_whitelisted(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="isWhitelisted", args=[100],
        box_references=[au.BoxReference(app_id=0, name=wl_key(100))],
        note=b"w2"))
    assert r.abi_return is False

def test_count_after_remove(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="getWhitelistCount", note=b"c3"))
    assert r.abi_return == 1

def test_batch_add(wl):
    wl.send.call(au.AppClientMethodCallParams(
        method="initWhitelistEntry", args=[300],
        box_references=[au.BoxReference(app_id=0, name=wl_key(300))]))
    wl.send.call(au.AppClientMethodCallParams(
        method="initWhitelistEntry", args=[400],
        box_references=[au.BoxReference(app_id=0, name=wl_key(400))]))
    wl.send.call(au.AppClientMethodCallParams(
        method="batchAdd", args=[300, 400],
        box_references=[
            au.BoxReference(app_id=0, name=wl_key(300)),
            au.BoxReference(app_id=0, name=wl_key(400)),
        ]))

def test_count_after_batch(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="getWhitelistCount", note=b"c4"))
    assert r.abi_return == 3

def test_batch_op_count(wl):
    r = wl.send.call(au.AppClientMethodCallParams(method="getBatchOpCount"))
    assert r.abi_return == 1

def test_batch_remove(wl):
    wl.send.call(au.AppClientMethodCallParams(
        method="batchRemove", args=[300, 400],
        box_references=[
            au.BoxReference(app_id=0, name=wl_key(300)),
            au.BoxReference(app_id=0, name=wl_key(400)),
        ]))

def test_count_after_batch_remove(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="getWhitelistCount", note=b"c5"))
    assert r.abi_return == 1

def test_batch_op_count_after(wl):
    r = wl.send.call(au.AppClientMethodCallParams(
        method="getBatchOpCount", note=b"b2"))
    assert r.abi_return == 2
