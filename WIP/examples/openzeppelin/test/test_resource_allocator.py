"""
ResourceAllocator behavioral tests.
Tests resource allocation, release, and tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def amount_key(aid):
    return mapping_box_key("_allocationAmount", aid.to_bytes(64, "big"))
def active_key(aid):
    return mapping_box_key("_allocationActive", aid.to_bytes(64, "big"))

def alloc_boxes(aid):
    return [
        au.BoxReference(app_id=0, name=amount_key(aid)),
        au.BoxReference(app_id=0, name=active_key(aid)),
    ]

@pytest.fixture(scope="module")
def ra(localnet, account):
    return deploy_contract(localnet, account, "ResourceAllocatorTest")

def test_deploy(ra):
    assert ra.app_id > 0

def test_admin(ra, account):
    r = ra.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_allocate(ra):
    boxes = alloc_boxes(0)
    ra.send.call(au.AppClientMethodCallParams(
        method="initAllocation", args=[0], box_references=boxes))
    r = ra.send.call(au.AppClientMethodCallParams(
        method="allocate", args=[500],
        box_references=boxes))
    assert r.abi_return == 0

def test_allocation_count(ra):
    r = ra.send.call(au.AppClientMethodCallParams(method="getAllocationCount"))
    assert r.abi_return == 1

def test_get_allocation(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="getAllocation", args=[0],
        box_references=[au.BoxReference(app_id=0, name=amount_key(0))]))
    assert r.abi_return == 500

def test_is_active(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="isActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_total_allocated(ra):
    r = ra.send.call(au.AppClientMethodCallParams(method="getTotalAllocated"))
    assert r.abi_return == 500

def test_allocate_second(ra):
    boxes = alloc_boxes(1)
    ra.send.call(au.AppClientMethodCallParams(
        method="initAllocation", args=[1], box_references=boxes))
    r = ra.send.call(au.AppClientMethodCallParams(
        method="allocate", args=[300],
        box_references=boxes, note=b"a2"))
    assert r.abi_return == 1

def test_total_after_second(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="getTotalAllocated", note=b"t2"))
    assert r.abi_return == 800

def test_release(ra):
    ra.send.call(au.AppClientMethodCallParams(
        method="release", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=active_key(0)),
            au.BoxReference(app_id=0, name=amount_key(0)),
        ]))

def test_not_active_after_release(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="isActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"ia2"))
    assert r.abi_return is False

def test_total_after_release(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="getTotalAllocated", note=b"t3"))
    assert r.abi_return == 300

def test_count_final(ra):
    r = ra.send.call(au.AppClientMethodCallParams(
        method="getAllocationCount", note=b"c2"))
    assert r.abi_return == 2
