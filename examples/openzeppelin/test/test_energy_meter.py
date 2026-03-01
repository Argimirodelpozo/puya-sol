"""
EnergyMeter behavioral tests.
Tests meter registration, reading recording, consumption tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def meter_hash_key(mid):
    return mapping_box_key("_meterHash", mid.to_bytes(64, "big"))
def consumption_key(mid):
    return mapping_box_key("_meterConsumption", mid.to_bytes(64, "big"))
def readings_key(mid):
    return mapping_box_key("_meterReadings", mid.to_bytes(64, "big"))

def meter_boxes(mid):
    return [
        au.BoxReference(app_id=0, name=meter_hash_key(mid)),
        au.BoxReference(app_id=0, name=consumption_key(mid)),
        au.BoxReference(app_id=0, name=readings_key(mid)),
    ]

@pytest.fixture(scope="module")
def em(localnet, account):
    return deploy_contract(localnet, account, "EnergyMeterTest")

def test_deploy(em):
    assert em.app_id > 0

def test_admin(em, account):
    r = em.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_meter(em):
    boxes = meter_boxes(0)
    em.send.call(au.AppClientMethodCallParams(
        method="initMeter", args=[0], box_references=boxes))
    r = em.send.call(au.AppClientMethodCallParams(
        method="registerMeter", args=[111],
        box_references=boxes))
    assert r.abi_return == 0

def test_meter_count(em):
    r = em.send.call(au.AppClientMethodCallParams(method="getMeterCount"))
    assert r.abi_return == 1

def test_meter_hash(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getMeterHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=meter_hash_key(0))]))
    assert r.abi_return == 111

def test_record_reading(em):
    em.send.call(au.AppClientMethodCallParams(
        method="recordReading", args=[0, 50],
        box_references=[
            au.BoxReference(app_id=0, name=consumption_key(0)),
            au.BoxReference(app_id=0, name=readings_key(0)),
        ]))

def test_reading_count(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getReadingCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=readings_key(0))]))
    assert r.abi_return == 1

def test_total_consumption(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getTotalConsumption", args=[0],
        box_references=[au.BoxReference(app_id=0, name=consumption_key(0))]))
    assert r.abi_return == 50

def test_record_more_readings(em):
    for i, val in enumerate([30, 70]):
        em.send.call(au.AppClientMethodCallParams(
            method="recordReading", args=[0, val],
            box_references=[
                au.BoxReference(app_id=0, name=consumption_key(0)),
                au.BoxReference(app_id=0, name=readings_key(0)),
            ], note=f"r{i}".encode()))

def test_reading_count_after(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getReadingCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=readings_key(0))],
        note=b"rc2"))
    assert r.abi_return == 3

def test_total_consumption_after(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getTotalConsumption", args=[0],
        box_references=[au.BoxReference(app_id=0, name=consumption_key(0))],
        note=b"tc2"))
    assert r.abi_return == 150  # 50 + 30 + 70

def test_global_consumption(em):
    r = em.send.call(au.AppClientMethodCallParams(method="getGlobalConsumption"))
    assert r.abi_return == 150

def test_register_second(em):
    boxes = meter_boxes(1)
    em.send.call(au.AppClientMethodCallParams(
        method="initMeter", args=[1], box_references=boxes))
    r = em.send.call(au.AppClientMethodCallParams(
        method="registerMeter", args=[222],
        box_references=boxes, note=b"m2"))
    assert r.abi_return == 1

def test_record_second_meter(em):
    em.send.call(au.AppClientMethodCallParams(
        method="recordReading", args=[1, 200],
        box_references=[
            au.BoxReference(app_id=0, name=consumption_key(1)),
            au.BoxReference(app_id=0, name=readings_key(1)),
        ]))

def test_global_consumption_final(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getGlobalConsumption", note=b"gc2"))
    assert r.abi_return == 350  # 150 + 200

def test_meter_count_final(em):
    r = em.send.call(au.AppClientMethodCallParams(
        method="getMeterCount", note=b"mc2"))
    assert r.abi_return == 2
