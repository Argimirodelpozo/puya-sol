"""
AccessList behavioral tests.
Tests role granting, revocation, expiry, and level management.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def role_key(addr):
    return mapping_box_key("_hasRole", encoding.decode_address(addr))


def expiry_key(addr):
    return mapping_box_key("_roleExpiry", encoding.decode_address(addr))


def level_key(addr):
    return mapping_box_key("_roleLevel", encoding.decode_address(addr))


def member_key(addr):
    return mapping_box_key("_memberIndex", encoding.decode_address(addr))


def member_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=role_key(addr)),
        au.BoxReference(app_id=0, name=expiry_key(addr)),
        au.BoxReference(app_id=0, name=level_key(addr)),
        au.BoxReference(app_id=0, name=member_key(addr)),
    ]


@pytest.fixture(scope="module")
def acl(localnet, account):
    return deploy_contract(localnet, account, "AccessListTest")


def test_deploy(acl):
    assert acl.app_id > 0


def test_admin(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_init_and_grant(acl, account):
    boxes = member_boxes(account.address)
    acl.send.call(
        au.AppClientMethodCallParams(
            method="initMember",
            args=[account.address],
            box_references=boxes,
        )
    )
    acl.send.call(
        au.AppClientMethodCallParams(
            method="grantRole",
            args=[account.address, 3, 0],  # level=3, expiry=0 (no expiry)
            box_references=boxes,
        )
    )


def test_has_role(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=role_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_role_level(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="getRoleLevel",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=level_key(account.address))],
        )
    )
    assert result.abi_return == 3


def test_is_active_no_expiry(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address, 99999],
            box_references=[
                au.BoxReference(app_id=0, name=role_key(account.address)),
                au.BoxReference(app_id=0, name=expiry_key(account.address)),
            ],
        )
    )
    assert result.abi_return is True


def test_member_count(acl):
    result = acl.send.call(
        au.AppClientMethodCallParams(method="getMemberCount")
    )
    assert result.abi_return == 1


def test_set_role_level(acl, account):
    acl.send.call(
        au.AppClientMethodCallParams(
            method="setRoleLevel",
            args=[account.address, 5],
            box_references=[au.BoxReference(app_id=0, name=level_key(account.address))],
        )
    )
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="getRoleLevel",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=level_key(account.address))],
            note=b"lv2",
        )
    )
    assert result.abi_return == 5


def test_extend_expiry(acl, account):
    acl.send.call(
        au.AppClientMethodCallParams(
            method="extendExpiry",
            args=[account.address, 5000],
            box_references=[au.BoxReference(app_id=0, name=expiry_key(account.address))],
        )
    )
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="getRoleExpiry",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=expiry_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_is_active_before_expiry(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address, 4000],  # before expiry
            box_references=[
                au.BoxReference(app_id=0, name=role_key(account.address)),
                au.BoxReference(app_id=0, name=expiry_key(account.address)),
            ],
            note=b"act2",
        )
    )
    assert result.abi_return is True


def test_is_active_after_expiry(acl, account):
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address, 6000],  # after expiry
            box_references=[
                au.BoxReference(app_id=0, name=role_key(account.address)),
                au.BoxReference(app_id=0, name=expiry_key(account.address)),
            ],
            note=b"act3",
        )
    )
    assert result.abi_return is False


def test_revoke_role(acl, account):
    acl.send.call(
        au.AppClientMethodCallParams(
            method="revokeRole",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=role_key(account.address))],
        )
    )
    result = acl.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=role_key(account.address))],
            note=b"hr2",
        )
    )
    assert result.abi_return is False
