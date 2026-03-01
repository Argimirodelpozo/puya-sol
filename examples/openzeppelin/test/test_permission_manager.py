"""
PermissionManager behavioral tests.
Tests role creation, permission grants/revokes, and queries.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def role_key(rid):
    return mapping_box_key("_roleHash", rid.to_bytes(64, "big"))
def perm_key(combined):
    return mapping_box_key("_permissions", combined.to_bytes(64, "big"))

@pytest.fixture(scope="module")
def pm(localnet, account):
    return deploy_contract(localnet, account, "PermissionManagerTest")

def test_deploy(pm):
    assert pm.app_id > 0

def test_admin(pm, account):
    r = pm.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_role(pm):
    pm.send.call(au.AppClientMethodCallParams(
        method="initRole", args=[0],
        box_references=[au.BoxReference(app_id=0, name=role_key(0))]))
    r = pm.send.call(au.AppClientMethodCallParams(
        method="createRole", args=[100],
        box_references=[au.BoxReference(app_id=0, name=role_key(0))]))
    assert r.abi_return == 0

def test_role_count(pm):
    r = pm.send.call(au.AppClientMethodCallParams(method="getRoleCount"))
    assert r.abi_return == 1

def test_role_hash(pm):
    r = pm.send.call(au.AppClientMethodCallParams(
        method="getRoleHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=role_key(0))]))
    assert r.abi_return == 100

def test_grant_permission(pm):
    # permKey = roleId(0) * 1000000 + permHash(1) = 1
    pk = 0 * 1000000 + 1
    pm.send.call(au.AppClientMethodCallParams(
        method="initPermission", args=[pk],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))
    pm.send.call(au.AppClientMethodCallParams(
        method="grantPermission", args=[0, 1],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))

def test_has_permission(pm):
    pk = 0 * 1000000 + 1
    r = pm.send.call(au.AppClientMethodCallParams(
        method="hasPermission", args=[0, 1],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))
    assert r.abi_return is True

def test_total_grants(pm):
    r = pm.send.call(au.AppClientMethodCallParams(method="getTotalGrants"))
    assert r.abi_return == 1

def test_grant_second_permission(pm):
    pk = 0 * 1000000 + 2
    pm.send.call(au.AppClientMethodCallParams(
        method="initPermission", args=[pk],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))
    pm.send.call(au.AppClientMethodCallParams(
        method="grantPermission", args=[0, 2],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))

def test_total_grants_after(pm):
    r = pm.send.call(au.AppClientMethodCallParams(
        method="getTotalGrants", note=b"g2"))
    assert r.abi_return == 2

def test_revoke_permission(pm):
    pk = 0 * 1000000 + 1
    pm.send.call(au.AppClientMethodCallParams(
        method="revokePermission", args=[0, 1],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))

def test_no_permission_after_revoke(pm):
    pk = 0 * 1000000 + 1
    r = pm.send.call(au.AppClientMethodCallParams(
        method="hasPermission", args=[0, 1],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))],
        note=b"h2"))
    assert r.abi_return is False

def test_other_permission_intact(pm):
    pk = 0 * 1000000 + 2
    r = pm.send.call(au.AppClientMethodCallParams(
        method="hasPermission", args=[0, 2],
        box_references=[au.BoxReference(app_id=0, name=perm_key(pk))]))
    assert r.abi_return is True

def test_create_second_role(pm):
    pm.send.call(au.AppClientMethodCallParams(
        method="initRole", args=[1],
        box_references=[au.BoxReference(app_id=0, name=role_key(1))]))
    pm.send.call(au.AppClientMethodCallParams(
        method="createRole", args=[200],
        box_references=[au.BoxReference(app_id=0, name=role_key(1))],
        note=b"r2"))

def test_role_count_after(pm):
    r = pm.send.call(au.AppClientMethodCallParams(
        method="getRoleCount", note=b"c2"))
    assert r.abi_return == 2
