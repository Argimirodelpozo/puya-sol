"""
AAVE V4 Roles library tests.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def roles(localnet, account):
    return deploy_contract(localnet, account, "RolesWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def test_deploy(roles):
    assert roles.app_id > 0


def test_default_admin_role(roles):
    assert _call(roles, "DEFAULT_ADMIN_ROLE") == 0


def test_hub_admin_role(roles):
    assert _call(roles, "HUB_ADMIN_ROLE") == 1


def test_spoke_admin_role(roles):
    assert _call(roles, "SPOKE_ADMIN_ROLE") == 2


def test_user_position_updater_role(roles):
    assert _call(roles, "USER_POSITION_UPDATER_ROLE") == 3


def test_hub_configurator_role(roles):
    assert _call(roles, "HUB_CONFIGURATOR_ROLE") == 4


def test_spoke_configurator_role(roles):
    assert _call(roles, "SPOKE_CONFIGURATOR_ROLE") == 5


def test_deficit_eliminator_role(roles):
    assert _call(roles, "DEFICIT_ELIMINATOR_ROLE") == 6


def test_roles_are_unique(roles):
    """All role values should be distinct."""
    values = set()
    for method in [
        "DEFAULT_ADMIN_ROLE", "HUB_ADMIN_ROLE", "SPOKE_ADMIN_ROLE",
        "USER_POSITION_UPDATER_ROLE", "HUB_CONFIGURATOR_ROLE",
        "SPOKE_CONFIGURATOR_ROLE", "DEFICIT_ELIMINATOR_ROLE"
    ]:
        v = _call(roles, method)
        assert v not in values, f"Duplicate role value: {v}"
        values.add(v)
