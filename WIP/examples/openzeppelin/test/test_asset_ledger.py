"""
AssetLedger behavioral tests.
Tests asset creation, transfer, and tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def owner_key(aid):
    return mapping_box_key("_assetOwner", aid.to_bytes(64, "big"))
def value_key(aid):
    return mapping_box_key("_assetValue", aid.to_bytes(64, "big"))
def xfer_key(aid):
    return mapping_box_key("_transferCount", aid.to_bytes(64, "big"))

def asset_boxes(aid):
    return [
        au.BoxReference(app_id=0, name=owner_key(aid)),
        au.BoxReference(app_id=0, name=value_key(aid)),
        au.BoxReference(app_id=0, name=xfer_key(aid)),
    ]

@pytest.fixture(scope="module")
def al(localnet, account):
    return deploy_contract(localnet, account, "AssetLedgerTest")

def test_deploy(al):
    assert al.app_id > 0

def test_admin(al, account):
    r = al.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_asset(al):
    boxes = asset_boxes(0)
    al.send.call(au.AppClientMethodCallParams(
        method="initAsset", args=[0], box_references=boxes))
    r = al.send.call(au.AppClientMethodCallParams(
        method="createAsset", args=[42, 10000],
        box_references=boxes))
    assert r.abi_return == 0

def test_asset_count(al):
    r = al.send.call(au.AppClientMethodCallParams(method="getAssetCount"))
    assert r.abi_return == 1

def test_asset_owner(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getAssetOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))]))
    assert r.abi_return == 42

def test_asset_value(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getAssetValue", args=[0],
        box_references=[au.BoxReference(app_id=0, name=value_key(0))]))
    assert r.abi_return == 10000

def test_initial_transfer_count(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getTransferCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=xfer_key(0))]))
    assert r.abi_return == 0

def test_transfer_asset(al):
    al.send.call(au.AppClientMethodCallParams(
        method="transferAsset", args=[0, 99],
        box_references=[
            au.BoxReference(app_id=0, name=owner_key(0)),
            au.BoxReference(app_id=0, name=xfer_key(0)),
        ]))

def test_owner_after_transfer(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getAssetOwner", args=[0],
        box_references=[au.BoxReference(app_id=0, name=owner_key(0))],
        note=b"o2"))
    assert r.abi_return == 99

def test_transfer_count_after(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getTransferCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=xfer_key(0))],
        note=b"tc2"))
    assert r.abi_return == 1

def test_total_transfers(al):
    r = al.send.call(au.AppClientMethodCallParams(method="getTotalTransfers"))
    assert r.abi_return == 1

def test_transfer_again(al):
    al.send.call(au.AppClientMethodCallParams(
        method="transferAsset", args=[0, 200],
        box_references=[
            au.BoxReference(app_id=0, name=owner_key(0)),
            au.BoxReference(app_id=0, name=xfer_key(0)),
        ], note=b"t2"))

def test_transfer_count_final(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getTransferCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=xfer_key(0))],
        note=b"tc3"))
    assert r.abi_return == 2

def test_total_transfers_final(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getTotalTransfers", note=b"tt2"))
    assert r.abi_return == 2

def test_create_second(al):
    boxes = asset_boxes(1)
    al.send.call(au.AppClientMethodCallParams(
        method="initAsset", args=[1], box_references=boxes))
    r = al.send.call(au.AppClientMethodCallParams(
        method="createAsset", args=[50, 5000],
        box_references=boxes, note=b"a2"))
    assert r.abi_return == 1

def test_asset_count_final(al):
    r = al.send.call(au.AppClientMethodCallParams(
        method="getAssetCount", note=b"ac2"))
    assert r.abi_return == 2
