"""
Allowlist behavioral tests.
Tests tiered allowlist with add/remove/tier operations.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def tier_key(addr):
    return mapping_box_key("_tiers", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def allowlist(localnet, account):
    return deploy_contract(localnet, account, "AllowlistTest")


def test_deploy(allowlist):
    assert allowlist.app_id > 0


def test_owner(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_count(allowlist):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(method="memberCount")
    )
    assert result.abi_return == 0


def test_set_tier_basic(allowlist, account):
    allowlist.send.call(
        au.AppClientMethodCallParams(
            method="setTier",
            args=[account.address, 1],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )


def test_is_allowed(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(
            method="isAllowed",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_is_not_premium(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(
            method="isPremium",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )
    assert result.abi_return is False


def test_member_count_after_add(allowlist):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(method="memberCount", note=b"c1")
    )
    assert result.abi_return == 1


def test_upgrade_to_premium(allowlist, account):
    allowlist.send.call(
        au.AppClientMethodCallParams(
            method="setTier",
            args=[account.address, 2],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
            note=b"upgrade",
        )
    )


def test_is_premium_after_upgrade(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(
            method="isPremium",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
            note=b"prem2",
        )
    )
    assert result.abi_return is True


def test_tier_of(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(
            method="tierOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )
    assert result.abi_return == 2


def test_count_unchanged_on_upgrade(allowlist):
    """Upgrading tier shouldn't change member count."""
    result = allowlist.send.call(
        au.AppClientMethodCallParams(method="memberCount", note=b"c2")
    )
    assert result.abi_return == 1


def test_remove_member(allowlist, account):
    allowlist.send.call(
        au.AppClientMethodCallParams(
            method="removeMember",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )


def test_not_allowed_after_remove(allowlist, account):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(
            method="isAllowed",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
            note=b"allow2",
        )
    )
    assert result.abi_return is False


def test_count_after_remove(allowlist):
    result = allowlist.send.call(
        au.AppClientMethodCallParams(method="memberCount", note=b"c3")
    )
    assert result.abi_return == 0
