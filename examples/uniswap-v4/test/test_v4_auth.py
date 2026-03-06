"""
Uniswap V4 — Auth Validation Tests

Tests that helper contracts with auth state initialized correctly
reject direct (ungrouped) calls and accept grouped calls.
"""
import pytest
import algokit_utils as au
from conftest import deploy_helper, init_auth_state
from helpers import grouped_call


@pytest.fixture(scope="module")
def auth_orchestrator(localnet, account):
    """Deploy a fresh orchestrator for auth tests."""
    return deploy_helper(localnet, account, "PoolManager")


@pytest.fixture(scope="module")
def auth_helper(localnet, account, auth_orchestrator):
    """Deploy a helper WITH auth state initialized."""
    helper = deploy_helper(localnet, account, "PoolManager__Helper44")
    init_auth_state(helper, auth_orchestrator.app_id)
    return helper


@pytest.fixture(scope="module")
def unauth_helper(localnet, account):
    """Deploy a helper WITHOUT auth state (o=0)."""
    return deploy_helper(localnet, account, "PoolManager__Helper44")


# --- Positive: auth-enabled helper accepts grouped call ---

@pytest.mark.localnet
def test_auth_grouped_call_succeeds(auth_helper, auth_orchestrator, algod_client, account):
    """Helper with auth state accepts calls within a group with orchestrator at position 0."""
    result = grouped_call(
        auth_helper, "BitMath.mostSignificantBit", [1],
        auth_orchestrator, algod_client, account,
    )
    assert result == 0


@pytest.mark.localnet
def test_auth_grouped_call_returns_correct_value(auth_helper, auth_orchestrator, algod_client, account):
    """Verify grouped call returns correct computation results."""
    result = grouped_call(
        auth_helper, "BitMath.mostSignificantBit", [256],
        auth_orchestrator, algod_client, account,
    )
    assert result == 8


# --- Negative: auth-enabled helper rejects ungrouped call ---

@pytest.mark.localnet
def test_auth_direct_call_rejected(auth_helper):
    """Helper with auth state rejects direct (ungrouped) calls."""
    with pytest.raises(Exception):
        auth_helper.send.call(au.AppClientMethodCallParams(
            method="BitMath.mostSignificantBit", args=[1],
        ))


# --- Baseline: unauth helper accepts direct call ---

@pytest.mark.localnet
def test_unauth_direct_call_accepted(unauth_helper):
    """Helper without auth state (o=0) accepts direct calls."""
    r = unauth_helper.send.call(au.AppClientMethodCallParams(
        method="BitMath.mostSignificantBit", args=[1],
    ))
    assert r.abi_return == 0
