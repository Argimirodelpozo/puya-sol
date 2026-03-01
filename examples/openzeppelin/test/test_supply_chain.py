"""
SupplyChain behavioral tests.
Tests shipment creation, transit, delivery, and returns.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def origin_key(sid):
    return mapping_box_key("_shipmentOrigin", sid.to_bytes(64, "big"))
def dest_key(sid):
    return mapping_box_key("_shipmentDestination", sid.to_bytes(64, "big"))
def status_key(sid):
    return mapping_box_key("_shipmentStatus", sid.to_bytes(64, "big"))
def time_key(sid):
    return mapping_box_key("_shipmentTimestamp", sid.to_bytes(64, "big"))

def shipment_boxes(sid):
    return [
        au.BoxReference(app_id=0, name=origin_key(sid)),
        au.BoxReference(app_id=0, name=dest_key(sid)),
        au.BoxReference(app_id=0, name=status_key(sid)),
        au.BoxReference(app_id=0, name=time_key(sid)),
    ]

@pytest.fixture(scope="module")
def sc(localnet, account):
    return deploy_contract(localnet, account, "SupplyChainTest")

def test_deploy(sc):
    assert sc.app_id > 0

def test_admin(sc, account):
    r = sc.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_shipment(sc):
    boxes = shipment_boxes(0)
    r = sc.send.call(au.AppClientMethodCallParams(
        method="createShipment", args=[1, 2, 1000],
        box_references=boxes))
    assert r.abi_return == 0

def test_shipment_count(sc):
    r = sc.send.call(au.AppClientMethodCallParams(method="getShipmentCount"))
    assert r.abi_return == 1

def test_origin(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="getShipmentOrigin", args=[0],
        box_references=[au.BoxReference(app_id=0, name=origin_key(0))]))
    assert r.abi_return == 1

def test_destination(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="getShipmentDestination", args=[0],
        box_references=[au.BoxReference(app_id=0, name=dest_key(0))]))
    assert r.abi_return == 2

def test_status_created(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="getShipmentStatus", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return == 0

def test_start_transit(sc):
    sc.send.call(au.AppClientMethodCallParams(
        method="startTransit", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))

def test_in_transit(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="isInTransit", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return is True

def test_deliver(sc):
    sc.send.call(au.AppClientMethodCallParams(
        method="deliver", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))

def test_is_delivered(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="isDelivered", args=[0],
        box_references=[au.BoxReference(app_id=0, name=status_key(0))]))
    assert r.abi_return is True

def test_delivered_count(sc):
    r = sc.send.call(au.AppClientMethodCallParams(method="getDeliveredCount"))
    assert r.abi_return == 1

def test_create_and_return(sc):
    boxes = shipment_boxes(1)
    sc.send.call(au.AppClientMethodCallParams(
        method="createShipment", args=[3, 4, 2000],
        box_references=boxes, note=b"s2"))
    sc.send.call(au.AppClientMethodCallParams(
        method="startTransit", args=[1],
        box_references=[au.BoxReference(app_id=0, name=status_key(1))],
        note=b"st2"))
    sc.send.call(au.AppClientMethodCallParams(
        method="returnShipment", args=[1],
        box_references=[au.BoxReference(app_id=0, name=status_key(1))]))

def test_is_returned(sc):
    r = sc.send.call(au.AppClientMethodCallParams(
        method="isReturned", args=[1],
        box_references=[au.BoxReference(app_id=0, name=status_key(1))]))
    assert r.abi_return is True

def test_returned_count(sc):
    r = sc.send.call(au.AppClientMethodCallParams(method="getReturnedCount"))
    assert r.abi_return == 1
