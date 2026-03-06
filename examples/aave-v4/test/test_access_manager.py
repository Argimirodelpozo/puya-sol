"""
AAVE V4 AccessManager tests.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
ADMIN_ROLE = 0

_call_counter = 0


@pytest.fixture(scope="module")
def manager(localnet, account):
    return deploy_contract(
        localnet, account, "AccessManager",
        extra_pages=2,
    )


def _call(client, method, *args):
    global _call_counter
    _call_counter += 1
    note = f"am_{_call_counter}".encode()
    result = client.send.call(
        au.AppClientMethodCallParams(method=method, args=list(args), note=note)
    )
    return result.abi_return


def test_deploy(manager):
    assert manager.app_id > 0


def test_min_setback(manager):
    result = _call(manager, "minSetback")
    assert result is not None
    assert isinstance(result, int)


def test_expiration(manager):
    """Expiration should be the EXPIRATION constant (1 week = 604800)."""
    result = _call(manager, "expiration")
    assert result == 604800


def test_isTargetClosed_default(manager, account):
    """Any target should not be closed by default."""
    result = _call(manager, "isTargetClosed", account.address)
    assert result in (False, 0)


def test_getRoleAdmin_default(manager):
    """Admin of any role should be 0 (ADMIN_ROLE) by default."""
    result = _call(manager, "getRoleAdmin", 1)
    assert result == 0


def test_getRoleGuardian_default(manager):
    """Guardian of any role should be 0 by default."""
    result = _call(manager, "getRoleGuardian", 1)
    assert result == 0


def test_getRoleGrantDelay_default(manager):
    """Grant delay for any role should be 0 by default."""
    result = _call(manager, "getRoleGrantDelay", 1)
    assert result == 0


def test_getTargetAdminDelay_default(manager, account):
    """Target admin delay should be 0 by default."""
    result = _call(manager, "getTargetAdminDelay", account.address)
    assert result == 0


def test_getTargetFunctionRole_default(manager, account):
    """Function role should default to ADMIN_ROLE (0)."""
    selector = [0, 0, 0, 0]
    result = _call(manager, "getTargetFunctionRole", account.address, selector)
    assert result == 0


def test_canCall_default(manager, account):
    """canCall should return (false, 0) for unconfigured callers."""
    selector = [0, 0, 0, 0]
    result = _call(manager, "canCall", account.address, account.address, selector)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[0] in (False, 0)


def test_hashOperation(manager, account):
    """hashOperation should return a deterministic bytes32."""
    data = b'\x00' * 4
    result = _call(manager, "hashOperation", account.address, account.address, data)
    assert result is not None
    if isinstance(result, (list, bytes)):
        assert len(result) == 32


def test_getNonce_default(manager):
    """Nonce for any operation should be 0 initially."""
    op_id = [0] * 32
    result = _call(manager, "getNonce", op_id)
    assert result == 0


def test_getSchedule_default(manager):
    """Schedule for any operation should be 0 initially."""
    op_id = [0] * 32
    result = _call(manager, "getSchedule", op_id)
    assert result == 0


def test_hasRole_unconfigured(manager, account):
    """hasRole for unconfigured role should return (false, 0)."""
    result = _call(manager, "hasRole", 99, account.address)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[0] in (False, 0)


def test_getAccess_unconfigured(manager, account):
    """getAccess for unconfigured role should return all zeros."""
    result = _call(manager, "getAccess", 99, account.address)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[0] == 0  # since
    assert vals[1] == 0  # currentDelay
