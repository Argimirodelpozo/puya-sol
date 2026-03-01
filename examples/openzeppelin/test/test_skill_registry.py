"""
SkillRegistry behavioral tests.
Tests skill registration, verification, level setting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def hash_key(sid):
    return mapping_box_key("_skillHash", sid.to_bytes(64, "big"))
def verified_key(sid):
    return mapping_box_key("_skillVerified", sid.to_bytes(64, "big"))
def level_key(sid):
    return mapping_box_key("_skillLevel", sid.to_bytes(64, "big"))

def skill_boxes(sid):
    return [
        au.BoxReference(app_id=0, name=hash_key(sid)),
        au.BoxReference(app_id=0, name=verified_key(sid)),
        au.BoxReference(app_id=0, name=level_key(sid)),
    ]

@pytest.fixture(scope="module")
def sr(localnet, account):
    return deploy_contract(localnet, account, "SkillRegistryTest")

def test_deploy(sr):
    assert sr.app_id > 0

def test_admin(sr, account):
    r = sr.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_skill(sr):
    boxes = skill_boxes(0)
    sr.send.call(au.AppClientMethodCallParams(
        method="initSkill", args=[0], box_references=boxes))
    r = sr.send.call(au.AppClientMethodCallParams(
        method="registerSkill", args=[42],
        box_references=boxes))
    assert r.abi_return == 0

def test_skill_count(sr):
    r = sr.send.call(au.AppClientMethodCallParams(method="getSkillCount"))
    assert r.abi_return == 1

def test_skill_hash(sr):
    r = sr.send.call(au.AppClientMethodCallParams(
        method="getSkillHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=hash_key(0))]))
    assert r.abi_return == 42

def test_not_verified_initially(sr):
    r = sr.send.call(au.AppClientMethodCallParams(
        method="isSkillVerified", args=[0],
        box_references=[au.BoxReference(app_id=0, name=verified_key(0))]))
    assert r.abi_return is False

def test_verify_skill(sr):
    sr.send.call(au.AppClientMethodCallParams(
        method="verifySkill", args=[0],
        box_references=[au.BoxReference(app_id=0, name=verified_key(0))]))

def test_is_verified(sr):
    r = sr.send.call(au.AppClientMethodCallParams(
        method="isSkillVerified", args=[0],
        box_references=[au.BoxReference(app_id=0, name=verified_key(0))],
        note=b"v2"))
    assert r.abi_return is True

def test_verified_count(sr):
    r = sr.send.call(au.AppClientMethodCallParams(method="getVerifiedCount"))
    assert r.abi_return == 1

def test_set_level(sr):
    sr.send.call(au.AppClientMethodCallParams(
        method="setSkillLevel", args=[0, 5],
        box_references=[au.BoxReference(app_id=0, name=level_key(0))]))

def test_get_level(sr):
    r = sr.send.call(au.AppClientMethodCallParams(
        method="getSkillLevel", args=[0],
        box_references=[au.BoxReference(app_id=0, name=level_key(0))]))
    assert r.abi_return == 5

def test_register_second(sr):
    boxes = skill_boxes(1)
    sr.send.call(au.AppClientMethodCallParams(
        method="initSkill", args=[1], box_references=boxes))
    r = sr.send.call(au.AppClientMethodCallParams(
        method="registerSkill", args=[99],
        box_references=boxes, note=b"s2"))
    assert r.abi_return == 1

def test_skill_count_final(sr):
    r = sr.send.call(au.AppClientMethodCallParams(
        method="getSkillCount", note=b"sc2"))
    assert r.abi_return == 2
