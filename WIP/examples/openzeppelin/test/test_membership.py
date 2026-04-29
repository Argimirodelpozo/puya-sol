"""
Membership behavioral tests.
Tests member management, fees, tiers, and suspension.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def tier_key(addr):
    return mapping_box_key("_memberTier", encoding.decode_address(addr))


def fees_key(addr):
    return mapping_box_key("_memberFeesPaid", encoding.decode_address(addr))


def member_key(addr):
    return mapping_box_key("_isMember", encoding.decode_address(addr))


def suspended_key(addr):
    return mapping_box_key("_isSuspended", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_memberIndex", encoding.decode_address(addr))


def member_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=tier_key(addr)),
        au.BoxReference(app_id=0, name=fees_key(addr)),
        au.BoxReference(app_id=0, name=member_key(addr)),
        au.BoxReference(app_id=0, name=suspended_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
    ]


@pytest.fixture(scope="module")
def mem(localnet, account):
    return deploy_contract(localnet, account, "MembershipTest")


def test_deploy(mem):
    assert mem.app_id > 0


def test_admin(mem, account):
    result = mem.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_fees(mem):
    r1 = mem.send.call(au.AppClientMethodCallParams(method="basicFee"))
    assert r1.abi_return == 100
    r2 = mem.send.call(au.AppClientMethodCallParams(method="premiumFee"))
    assert r2.abi_return == 500
    r3 = mem.send.call(au.AppClientMethodCallParams(method="vipFee"))
    assert r3.abi_return == 1000


def test_init_and_add_member(mem, account):
    boxes = member_boxes(account.address)
    mem.send.call(
        au.AppClientMethodCallParams(
            method="initMember",
            args=[account.address],
            box_references=boxes,
        )
    )
    mem.send.call(
        au.AppClientMethodCallParams(
            method="addMember",
            args=[account.address, 0],  # basic tier
            box_references=boxes,
        )
    )


def test_is_member(mem, account):
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="isMember",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=member_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_member_count(mem):
    result = mem.send.call(
        au.AppClientMethodCallParams(method="memberCount")
    )
    assert result.abi_return == 1


def test_member_tier(mem, account):
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="getMemberTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
        )
    )
    assert result.abi_return == 0  # basic


def test_get_fee_for_tier(mem):
    r = mem.send.call(
        au.AppClientMethodCallParams(
            method="getFeeForTier",
            args=[0],
        )
    )
    assert r.abi_return == 100


def test_pay_fee(mem, account):
    mem.send.call(
        au.AppClientMethodCallParams(
            method="payFee",
            args=[account.address, 100],
            box_references=[
                au.BoxReference(app_id=0, name=member_key(account.address)),
                au.BoxReference(app_id=0, name=suspended_key(account.address)),
                au.BoxReference(app_id=0, name=fees_key(account.address)),
            ],
        )
    )


def test_fees_paid(mem, account):
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="getMemberFeesPaid",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=fees_key(account.address))],
        )
    )
    assert result.abi_return == 100


def test_total_fees(mem):
    result = mem.send.call(
        au.AppClientMethodCallParams(method="totalFees")
    )
    assert result.abi_return == 100


def test_upgrade_tier(mem, account):
    mem.send.call(
        au.AppClientMethodCallParams(
            method="upgradeTier",
            args=[account.address, 1],  # premium
            box_references=[
                au.BoxReference(app_id=0, name=member_key(account.address)),
                au.BoxReference(app_id=0, name=tier_key(account.address)),
            ],
        )
    )
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="getMemberTier",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=tier_key(account.address))],
            note=b"t2",
        )
    )
    assert result.abi_return == 1  # premium


def test_suspend(mem, account):
    mem.send.call(
        au.AppClientMethodCallParams(
            method="suspend",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=suspended_key(account.address))],
        )
    )
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="isSuspended",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=suspended_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_reinstate(mem, account):
    mem.send.call(
        au.AppClientMethodCallParams(
            method="reinstate",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=suspended_key(account.address))],
        )
    )
    result = mem.send.call(
        au.AppClientMethodCallParams(
            method="isSuspended",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=suspended_key(account.address))],
            note=b"s2",
        )
    )
    assert result.abi_return is False
