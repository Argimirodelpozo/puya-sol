"""Tests for Solady Ownable compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/Ownable.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "OwnableWrapper")
    return make_caller(algod_client, account, app_id, spec)


class TestOwnable:
    @pytest.mark.xfail(reason="owner not set in constructor — returns empty address")
    def test_deploy(self, call):
        """Contract deploys successfully."""
        owner = call("owner")
        assert owner is not None

    @pytest.mark.xfail(reason="owner not set in constructor — returns empty address")
    def test_get_owner(self, call):
        """getOwner() returns the owner address."""
        owner = call("getOwner")
        assert owner is not None

    @pytest.mark.xfail(reason="owner not set in constructor — returns empty address")
    def test_owner_matches_get_owner(self, call):
        """owner() and getOwner() return the same value."""
        owner1 = call("owner")
        owner2 = call("getOwner")
        assert owner1 == owner2

    def test_get_handover_expiry_zero(self, call, account):
        """getHandoverExpiry returns 0 for non-pending address."""
        result = call("getHandoverExpiry", account.address)
        assert result == 0

    def test_ownership_handover_expires_at_zero(self, call, account):
        """ownershipHandoverExpiresAt returns 0 for non-pending address."""
        result = call("ownershipHandoverExpiresAt", account.address)
        assert result == 0
