"""
Allowance behavioral tests.
Tests account setup, spending, limits, and period resets.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def limit_key(addr):
    return mapping_box_key("_limit", encoding.decode_address(addr))


def spent_key(addr):
    return mapping_box_key("_spent", encoding.decode_address(addr))


def acct_idx_key(addr):
    return mapping_box_key("_accountIndex", encoding.decode_address(addr))


def active_key(addr):
    return mapping_box_key("_isActive", encoding.decode_address(addr))


def acct_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=limit_key(addr)),
        au.BoxReference(app_id=0, name=spent_key(addr)),
        au.BoxReference(app_id=0, name=acct_idx_key(addr)),
        au.BoxReference(app_id=0, name=active_key(addr)),
    ]


@pytest.fixture(scope="module")
def allow(localnet, account):
    return deploy_contract(localnet, account, "AllowanceTest")


def test_deploy(allow):
    assert allow.app_id > 0


def test_admin(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_setup_account(allow, account):
    boxes = acct_boxes(account.address)
    allow.send.call(
        au.AppClientMethodCallParams(
            method="initAccount",
            args=[account.address],
            box_references=boxes,
        )
    )
    allow.send.call(
        au.AppClientMethodCallParams(
            method="setupAccount",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )


def test_account_count(allow):
    result = allow.send.call(
        au.AppClientMethodCallParams(method="accountCount")
    )
    assert result.abi_return == 1


def test_limit(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getLimit",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=limit_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_is_active(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=active_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_remaining(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getRemaining",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=limit_key(account.address)),
                au.BoxReference(app_id=0, name=spent_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 5000


def test_total_budget(allow):
    result = allow.send.call(
        au.AppClientMethodCallParams(method="totalBudget")
    )
    assert result.abi_return == 5000


def test_spend(allow, account):
    allow.send.call(
        au.AppClientMethodCallParams(
            method="spend",
            args=[account.address, 2000],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(account.address)),
                au.BoxReference(app_id=0, name=spent_key(account.address)),
                au.BoxReference(app_id=0, name=limit_key(account.address)),
            ],
        )
    )


def test_spent(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getSpent",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=spent_key(account.address))],
        )
    )
    assert result.abi_return == 2000


def test_remaining_after_spend(allow, account):
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getRemaining",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=limit_key(account.address)),
                au.BoxReference(app_id=0, name=spent_key(account.address)),
            ],
            note=b"r2",
        )
    )
    assert result.abi_return == 3000


def test_total_spent(allow):
    result = allow.send.call(
        au.AppClientMethodCallParams(method="totalSpent")
    )
    assert result.abi_return == 2000


def test_reset_period(allow, account):
    allow.send.call(
        au.AppClientMethodCallParams(
            method="resetPeriod",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=spent_key(account.address))],
        )
    )
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getSpent",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=spent_key(account.address))],
            note=b"sp2",
        )
    )
    assert result.abi_return == 0


def test_update_limit(allow, account):
    allow.send.call(
        au.AppClientMethodCallParams(
            method="updateLimit",
            args=[account.address, 8000],
            box_references=[au.BoxReference(app_id=0, name=limit_key(account.address))],
        )
    )
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="getLimit",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=limit_key(account.address))],
            note=b"l2",
        )
    )
    assert result.abi_return == 8000


def test_total_budget_after_update(allow):
    # 5000 → 8000, so budget increased by 3000
    result = allow.send.call(
        au.AppClientMethodCallParams(method="totalBudget", note=b"tb2")
    )
    assert result.abi_return == 8000


def test_deactivate(allow, account):
    allow.send.call(
        au.AppClientMethodCallParams(
            method="deactivateAccount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=active_key(account.address))],
        )
    )
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=active_key(account.address))],
            note=b"a2",
        )
    )
    assert result.abi_return is False


def test_activate(allow, account):
    allow.send.call(
        au.AppClientMethodCallParams(
            method="activateAccount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=active_key(account.address))],
        )
    )
    result = allow.send.call(
        au.AppClientMethodCallParams(
            method="isActive",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=active_key(account.address))],
            note=b"a3",
        )
    )
    assert result.abi_return is True
