"""
Whitelist behavioral tests.
Tests add/remove/query/batch whitelist operations.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def wl_key(addr):
    return mapping_box_key("_whitelisted", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def whitelist(localnet, account):
    return deploy_contract(localnet, account, "WhitelistTest")


def test_deploy(whitelist):
    assert whitelist.app_id > 0


def test_admin(whitelist, account):
    result = whitelist.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_initial_count(whitelist):
    result = whitelist.send.call(
        au.AppClientMethodCallParams(method="whitelistCount")
    )
    assert result.abi_return == 0


def test_add_to_whitelist(whitelist, account):
    key = wl_key(account.address)
    whitelist.send.call(
        au.AppClientMethodCallParams(
            method="addToWhitelist",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_is_whitelisted(whitelist, account):
    key = wl_key(account.address)
    result = whitelist.send.call(
        au.AppClientMethodCallParams(
            method="isWhitelisted",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )
    assert result.abi_return is True


def test_count_after_add(whitelist):
    result = whitelist.send.call(
        au.AppClientMethodCallParams(method="whitelistCount", note=b"c1")
    )
    assert result.abi_return == 1


def test_remove_from_whitelist(whitelist, account):
    key = wl_key(account.address)
    whitelist.send.call(
        au.AppClientMethodCallParams(
            method="removeFromWhitelist",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
        )
    )


def test_not_whitelisted_after_remove(whitelist, account):
    key = wl_key(account.address)
    result = whitelist.send.call(
        au.AppClientMethodCallParams(
            method="isWhitelisted",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=key)],
            note=b"check2",
        )
    )
    assert result.abi_return is False


def test_count_after_remove(whitelist):
    result = whitelist.send.call(
        au.AppClientMethodCallParams(method="whitelistCount", note=b"c2")
    )
    assert result.abi_return == 0
