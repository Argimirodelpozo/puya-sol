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
