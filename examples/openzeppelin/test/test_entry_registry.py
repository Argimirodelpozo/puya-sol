"""
EntryRegistry behavioral tests.
Tests entry registration, ownership, activation, and transfers.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def owner_key(eid):
    return mapping_box_key("_entryOwner", eid.to_bytes(64, "big"))
def active_key(eid):
    return mapping_box_key("_entryActive", eid.to_bytes(64, "big"))
def name_hash_key(eid):
    return mapping_box_key("_entryNameHash", eid.to_bytes(64, "big"))

def entry_boxes(eid):
    return [
        au.BoxReference(app_id=0, name=owner_key(eid)),
        au.BoxReference(app_id=0, name=active_key(eid)),
        au.BoxReference(app_id=0, name=name_hash_key(eid)),
    ]

@pytest.fixture(scope="module")
def reg(localnet, account):
    return deploy_contract(localnet, account, "EntryRegistryTest")

def test_deploy(reg):
    assert reg.app_id > 0

def test_admin(reg, account):
    r = reg.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_entry(reg):
    boxes = entry_boxes(0)
    reg.send.call(au.AppClientMethodCallParams(
        method="initEntry", args=[0], box_references=boxes))
    r = reg.send.call(au.AppClientMethodCallParams(
        method="registerEntry", args=[42],
        box_references=boxes))
    assert r.abi_return == 0

def test_entry_count(reg):
    r = reg.send.call(au.AppClientMethodCallParams(method="getEntryCount"))
    assert r.abi_return == 1

def test_entry_active(reg):
    r = reg.send.call(au.AppClientMethodCallParams(
        method="isEntryActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_entry_owner(reg, account):
    r = reg.send.call(au.AppClientMethodCallParams(
        method="getEntryOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    assert r.abi_return == account.address

def test_entry_name_hash(reg):
    r = reg.send.call(au.AppClientMethodCallParams(
        method="getEntryNameHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=name_hash_key(0))]))
    assert r.abi_return == 42

def test_deactivate_entry(reg):
    reg.send.call(au.AppClientMethodCallParams(
        method="deactivateEntry", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = reg.send.call(au.AppClientMethodCallParams(
        method="isEntryActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is False

def test_activate_entry(reg):
    reg.send.call(au.AppClientMethodCallParams(
        method="activateEntry", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = reg.send.call(au.AppClientMethodCallParams(
        method="isEntryActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a3"))
    assert r.abi_return is True

def test_transfer_entry(reg):
    # Transfer to a known address (zero address padded)
    target = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
    reg.send.call(au.AppClientMethodCallParams(
        method="transferEntry", args=[0, target],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    r = reg.send.call(au.AppClientMethodCallParams(
        method="getEntryOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))],
        note=b"o2"))
    assert r.abi_return == target

def test_register_second(reg):
    boxes = entry_boxes(1)
    reg.send.call(au.AppClientMethodCallParams(
        method="initEntry", args=[1], box_references=boxes))
    r = reg.send.call(au.AppClientMethodCallParams(
        method="registerEntry", args=[99],
        box_references=boxes, note=b"r2"))
    assert r.abi_return == 1

def test_count_after(reg):
    r = reg.send.call(au.AppClientMethodCallParams(
        method="getEntryCount", note=b"c2"))
    assert r.abi_return == 2
