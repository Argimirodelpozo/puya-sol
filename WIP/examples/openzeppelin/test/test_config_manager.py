"""
ConfigManager behavioral tests.
Tests config set/get, existence checks, and update counting.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def value_key(ck):
    return mapping_box_key("_configValue", ck.to_bytes(64, "big"))
def exists_key(ck):
    return mapping_box_key("_configExists", ck.to_bytes(64, "big"))

def config_boxes(ck):
    return [
        au.BoxReference(app_id=0, name=value_key(ck)),
        au.BoxReference(app_id=0, name=exists_key(ck)),
    ]

@pytest.fixture(scope="module")
def cfg(localnet, account):
    return deploy_contract(localnet, account, "ConfigManagerTest")

def test_deploy(cfg):
    assert cfg.app_id > 0

def test_admin(cfg, account):
    r = cfg.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_set_config(cfg):
    boxes = config_boxes(1)
    cfg.send.call(au.AppClientMethodCallParams(
        method="initConfig", args=[1], box_references=boxes))
    cfg.send.call(au.AppClientMethodCallParams(
        method="setConfig", args=[1, 42],
        box_references=boxes))

def test_config_count(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(method="getConfigCount"))
    assert r.abi_return == 1

def test_update_count(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(method="getUpdateCount"))
    assert r.abi_return == 1

def test_get_config(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getConfig", args=[1],
        box_references=[au.BoxReference(app_id=0, name=value_key(1))]))
    assert r.abi_return == 42

def test_is_config_set(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="isConfigSet", args=[1],
        box_references=[au.BoxReference(app_id=0, name=exists_key(1))]))
    assert r.abi_return is True

def test_update_existing(cfg):
    cfg.send.call(au.AppClientMethodCallParams(
        method="setConfig", args=[1, 99],
        box_references=config_boxes(1), note=b"s2"))

def test_config_count_unchanged(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getConfigCount", note=b"c2"))
    assert r.abi_return == 1  # same key, no new count

def test_update_count_incremented(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getUpdateCount", note=b"u2"))
    assert r.abi_return == 2

def test_get_updated_value(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getConfig", args=[1],
        box_references=[au.BoxReference(app_id=0, name=value_key(1))],
        note=b"g2"))
    assert r.abi_return == 99

def test_set_second_key(cfg):
    boxes = config_boxes(2)
    cfg.send.call(au.AppClientMethodCallParams(
        method="initConfig", args=[2], box_references=boxes))
    cfg.send.call(au.AppClientMethodCallParams(
        method="setConfig", args=[2, 77],
        box_references=boxes, note=b"s3"))

def test_config_count_after(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getConfigCount", note=b"c3"))
    assert r.abi_return == 2

def test_update_count_after(cfg):
    r = cfg.send.call(au.AppClientMethodCallParams(
        method="getUpdateCount", note=b"u3"))
    assert r.abi_return == 3
