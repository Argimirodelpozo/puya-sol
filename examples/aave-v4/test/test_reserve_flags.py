"""
AAVE V4 ReserveFlagsMap library tests.
Translated from ReserveFlags.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

PAUSED_MASK = 0x01
FROZEN_MASK = 0x02
BORROWABLE_MASK = 0x04
RECEIVE_SHARES_ENABLED_MASK = 0x08


@pytest.fixture(scope="module")
def flags(localnet, account):
    return deploy_contract(localnet, account, "ReserveFlagsMapWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


# ─── Constants ─────────────────────────────────────────────────────────────────

def test_deploy(flags):
    assert flags.app_id > 0


def test_paused_mask(flags):
    assert _call(flags, "PAUSED_MASK") == PAUSED_MASK


def test_frozen_mask(flags):
    assert _call(flags, "FROZEN_MASK") == FROZEN_MASK


def test_borrowable_mask(flags):
    assert _call(flags, "BORROWABLE_MASK") == BORROWABLE_MASK


def test_receive_shares_enabled_mask(flags):
    assert _call(flags, "RECEIVE_SHARES_ENABLED_MASK") == RECEIVE_SHARES_ENABLED_MASK


# ─── create ────────────────────────────────────────────────────────────────────

def test_create_all_false(flags):
    result = _call(flags, "create", False, False, False, False)
    assert result == 0


def test_create_all_true(flags):
    result = _call(flags, "create", True, True, True, True)
    assert result == PAUSED_MASK | FROZEN_MASK | BORROWABLE_MASK | RECEIVE_SHARES_ENABLED_MASK


def test_create_paused_only(flags):
    result = _call(flags, "create", True, False, False, False)
    assert result == PAUSED_MASK


def test_create_frozen_only(flags):
    result = _call(flags, "create", False, True, False, False)
    assert result == FROZEN_MASK


def test_create_borrowable_only(flags):
    result = _call(flags, "create", False, False, True, False)
    assert result == BORROWABLE_MASK


def test_create_receive_shares_only(flags):
    result = _call(flags, "create", False, False, False, True)
    assert result == RECEIVE_SHARES_ENABLED_MASK


# ─── setters/getters ──────────────────────────────────────────────────────────

def test_set_and_get_paused(flags):
    f = _call(flags, "create", False, False, False, False)
    assert _call(flags, "paused", f) == False
    f = _call(flags, "setPaused", f, True)
    assert _call(flags, "paused", f) == True
    f = _call(flags, "setPaused", f, False)
    assert _call(flags, "paused", f) == False


def test_set_and_get_frozen(flags):
    f = _call(flags, "create", False, False, False, False)
    assert _call(flags, "frozen", f) == False
    f = _call(flags, "setFrozen", f, True)
    assert _call(flags, "frozen", f) == True


def test_set_and_get_borrowable(flags):
    f = _call(flags, "create", False, False, False, False)
    assert _call(flags, "borrowable", f) == False
    f = _call(flags, "setBorrowable", f, True)
    assert _call(flags, "borrowable", f) == True


def test_set_and_get_receive_shares(flags):
    f = _call(flags, "create", False, False, False, False)
    assert _call(flags, "receiveSharesEnabled", f) == False
    f = _call(flags, "setReceiveSharesEnabled", f, True)
    assert _call(flags, "receiveSharesEnabled", f) == True


# ─── Independence ─────────────────────────────────────────────────────────────

def test_flags_independence(flags):
    """Setting one flag should not affect others."""
    f = _call(flags, "create", False, False, False, False)
    f = _call(flags, "setPaused", f, True)
    assert _call(flags, "paused", f) == True
    assert _call(flags, "frozen", f) == False
    assert _call(flags, "borrowable", f) == False
    assert _call(flags, "receiveSharesEnabled", f) == False

    f = _call(flags, "setFrozen", f, True)
    assert _call(flags, "paused", f) == True
    assert _call(flags, "frozen", f) == True
    assert _call(flags, "borrowable", f) == False
    assert _call(flags, "receiveSharesEnabled", f) == False


def test_clear_flag(flags):
    """Setting a flag to false should clear only that flag."""
    f = _call(flags, "create", True, True, True, True)
    f = _call(flags, "setPaused", f, False)
    assert _call(flags, "paused", f) == False
    assert _call(flags, "frozen", f) == True
    assert _call(flags, "borrowable", f) == True
    assert _call(flags, "receiveSharesEnabled", f) == True
