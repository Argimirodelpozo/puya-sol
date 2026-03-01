"""
RewardPool behavioral tests.
Tests recipient management, reward distribution, and claiming.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def rhash_key(rid):
    return mapping_box_key("_recipientHash", rid.to_bytes(64, "big"))
def pending_key(rid):
    return mapping_box_key("_pendingReward", rid.to_bytes(64, "big"))
def claimed_key(rid):
    return mapping_box_key("_claimedReward", rid.to_bytes(64, "big"))

def recipient_boxes(rid):
    return [
        au.BoxReference(app_id=0, name=rhash_key(rid)),
        au.BoxReference(app_id=0, name=pending_key(rid)),
        au.BoxReference(app_id=0, name=claimed_key(rid)),
    ]

@pytest.fixture(scope="module")
def rp(localnet, account):
    return deploy_contract(localnet, account, "RewardPoolTest")

def test_deploy(rp):
    assert rp.app_id > 0

def test_admin(rp, account):
    r = rp.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_add_recipient(rp):
    boxes = recipient_boxes(0)
    rp.send.call(au.AppClientMethodCallParams(
        method="initRecipient", args=[0], box_references=boxes))
    r = rp.send.call(au.AppClientMethodCallParams(
        method="addRecipient", args=[333],
        box_references=boxes))
    assert r.abi_return == 0

def test_recipient_count(rp):
    r = rp.send.call(au.AppClientMethodCallParams(method="getRecipientCount"))
    assert r.abi_return == 1

def test_recipient_hash(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getRecipientHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rhash_key(0))]))
    assert r.abi_return == 333

def test_distribute_reward(rp):
    rp.send.call(au.AppClientMethodCallParams(
        method="distributeReward", args=[0, 1000],
        box_references=[au.BoxReference(app_id=0, name=pending_key(0))]))

def test_pending_reward(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getPendingReward", args=[0],
        box_references=[au.BoxReference(app_id=0, name=pending_key(0))]))
    assert r.abi_return == 1000

def test_total_distributed(rp):
    r = rp.send.call(au.AppClientMethodCallParams(method="getTotalDistributed"))
    assert r.abi_return == 1000

def test_distribute_more(rp):
    rp.send.call(au.AppClientMethodCallParams(
        method="distributeReward", args=[0, 500],
        box_references=[au.BoxReference(app_id=0, name=pending_key(0))],
        note=b"d2"))

def test_pending_after_more(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getPendingReward", args=[0],
        box_references=[au.BoxReference(app_id=0, name=pending_key(0))],
        note=b"p2"))
    assert r.abi_return == 1500

def test_claim_reward(rp):
    rp.send.call(au.AppClientMethodCallParams(
        method="claimReward", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=pending_key(0)),
            au.BoxReference(app_id=0, name=claimed_key(0)),
        ]))

def test_pending_after_claim(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getPendingReward", args=[0],
        box_references=[au.BoxReference(app_id=0, name=pending_key(0))],
        note=b"p3"))
    assert r.abi_return == 0

def test_claimed_reward(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getClaimedReward", args=[0],
        box_references=[au.BoxReference(app_id=0, name=claimed_key(0))]))
    assert r.abi_return == 1500

def test_total_claimed(rp):
    r = rp.send.call(au.AppClientMethodCallParams(method="getTotalClaimed"))
    assert r.abi_return == 1500

def test_total_distributed_final(rp):
    r = rp.send.call(au.AppClientMethodCallParams(
        method="getTotalDistributed", note=b"td2"))
    assert r.abi_return == 1500
