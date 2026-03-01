"""
AssetTracker behavioral tests.
Tests asset registration, transfer, depreciation, and value tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def val_key(aid):
    return mapping_box_key("_assetValue", aid.to_bytes(64, "big"))
def owner_key(aid):
    return mapping_box_key("_assetOwnerHash", aid.to_bytes(64, "big"))

def asset_boxes(aid):
    return [
        au.BoxReference(app_id=0, name=val_key(aid)),
        au.BoxReference(app_id=0, name=owner_key(aid)),
    ]

@pytest.fixture(scope="module")
def at(localnet, account):
    return deploy_contract(localnet, account, "AssetTrackerTest")

def test_deploy(at):
    assert at.app_id > 0

def test_admin(at, account):
    r = at.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_asset(at):
    boxes = asset_boxes(0)
    at.send.call(au.AppClientMethodCallParams(
        method="initAsset", args=[0], box_references=boxes))
    r = at.send.call(au.AppClientMethodCallParams(
        method="registerAsset", args=[10000],
        box_references=boxes))
    assert r.abi_return == 0

def test_asset_count(at):
    r = at.send.call(au.AppClientMethodCallParams(method="getAssetCount"))
    assert r.abi_return == 1

def test_asset_value(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAssetValue", args=[0],
        box_references=[au.BoxReference(app_id=0, name=val_key(0))]))
    assert r.abi_return == 10000

def test_total_value(at):
    r = at.send.call(au.AppClientMethodCallParams(method="getTotalValue"))
    assert r.abi_return == 10000

def test_transfer_asset(at):
    at.send.call(au.AppClientMethodCallParams(
        method="transferAsset", args=[0, 999],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))

def test_owner_hash(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAssetOwnerHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    assert r.abi_return == 999

def test_depreciate(at):
    at.send.call(au.AppClientMethodCallParams(
        method="depreciateAsset", args=[0, 3000],
        box_references=[au.BoxReference(app_id=0, name=val_key(0))]))

def test_value_after_depreciation(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getAssetValue", args=[0],
        box_references=[au.BoxReference(app_id=0, name=val_key(0))],
        note=b"v2"))
    assert r.abi_return == 7000

def test_total_value_after(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getTotalValue", note=b"t2"))
    assert r.abi_return == 7000

def test_register_second(at):
    boxes = asset_boxes(1)
    at.send.call(au.AppClientMethodCallParams(
        method="initAsset", args=[1], box_references=boxes))
    r = at.send.call(au.AppClientMethodCallParams(
        method="registerAsset", args=[5000],
        box_references=boxes, note=b"r2"))
    assert r.abi_return == 1

def test_total_value_final(at):
    r = at.send.call(au.AppClientMethodCallParams(
        method="getTotalValue", note=b"t3"))
    assert r.abi_return == 12000
