"""
AAVE V4 ConfigPermissionsMap library tests.
Translated from ConfigPermissions.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def perms(localnet, account):
    return deploy_contract(localnet, account, "ConfigPermissionsMapWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def test_deploy(perms):
    assert perms.app_id > 0


def test_setFullPermissions_true(perms):
    result = _call(perms, "setFullPermissions", True)
    # All 7 permission bits should be set
    assert result == 0x7  # 3 permission bits


def test_setFullPermissions_false(perms):
    result = _call(perms, "setFullPermissions", False)
    assert result == 0


def test_individual_permissions(perms):
    # Start with no permissions
    p = 0
    # Set canSetUsingAsCollateral
    p = _call(perms, "setCanSetUsingAsCollateral", p, True)
    assert _call(perms, "canSetUsingAsCollateral", p) == True
    assert _call(perms, "canUpdateUserRiskPremium", p) == False
    assert _call(perms, "canUpdateUserDynamicConfig", p) == False


def test_setCanUpdateUserRiskPremium(perms):
    p = 0
    p = _call(perms, "setCanUpdateUserRiskPremium", p, True)
    assert _call(perms, "canUpdateUserRiskPremium", p) == True
    assert _call(perms, "canSetUsingAsCollateral", p) == False


def test_setCanUpdateUserDynamicConfig(perms):
    p = 0
    p = _call(perms, "setCanUpdateUserDynamicConfig", p, True)
    assert _call(perms, "canUpdateUserDynamicConfig", p) == True
    assert _call(perms, "canSetUsingAsCollateral", p) == False


def test_eq_same(perms):
    assert _call(perms, "eq", 0x7F, 0x7F) == True


def test_eq_different(perms):
    assert _call(perms, "eq", 0x01, 0x02) == False


def test_eq_zero(perms):
    assert _call(perms, "eq", 0, 0) == True


def test_clear_permission(perms):
    """Setting a permission to false should clear it."""
    p = _call(perms, "setFullPermissions", True)
    p = _call(perms, "setCanSetUsingAsCollateral", p, False)
    assert _call(perms, "canSetUsingAsCollateral", p) == False
    # Other permissions should still be set
    assert _call(perms, "canUpdateUserRiskPremium", p) == True
    assert _call(perms, "canUpdateUserDynamicConfig", p) == True


def test_getConfigPermissionValues(perms):
    """Direct port of upstream test_getConfigPermissionValues — for
    each of the 8 sanitized permission values (0..7, masked by
    GLOBAL_PERMISSIONS_MASK), the struct returned by
    getConfigPermissionValues should match the individual canX getters.

    Upstream uses fuzz over uint8; we exhaust the 3-bit space directly."""
    GLOBAL_MASK = 0x7
    for p in range(GLOBAL_MASK + 1):
        values = _call(perms, "getConfigPermissionValues", p)
        # ARC56 struct decode: algokit returns dict-of-named-fields if the
        # arc56 spec carries names; otherwise a tuple.
        def _field(key, idx):
            return values[key] if isinstance(values, dict) else values[idx]
        assert _field("canSetUsingAsCollateral", 0) == \
            _call(perms, "canSetUsingAsCollateral", p), \
            f"canSetUsingAsCollateral mismatch at p={p:#x}"
        assert _field("canUpdateUserRiskPremium", 1) == \
            _call(perms, "canUpdateUserRiskPremium", p), \
            f"canUpdateUserRiskPremium mismatch at p={p:#x}"
        assert _field("canUpdateUserDynamicConfig", 2) == \
            _call(perms, "canUpdateUserDynamicConfig", p), \
            f"canUpdateUserDynamicConfig mismatch at p={p:#x}"
