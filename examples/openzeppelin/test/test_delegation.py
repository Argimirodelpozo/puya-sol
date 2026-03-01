"""
Delegation behavioral tests.
Tests registration, delegation, undelegation, and effective power.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def power_key(addr):
    return mapping_box_key("_power", encoding.decode_address(addr))


def delegate_key(addr):
    return mapping_box_key("_delegateTo", encoding.decode_address(addr))


def delegated_power_key(addr):
    return mapping_box_key("_delegatedPower", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_userIndex", encoding.decode_address(addr))


def registered_key(addr):
    return mapping_box_key("_isRegistered", encoding.decode_address(addr))


def user_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=power_key(addr)),
        au.BoxReference(app_id=0, name=delegate_key(addr)),
        au.BoxReference(app_id=0, name=delegated_power_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
        au.BoxReference(app_id=0, name=registered_key(addr)),
    ]


@pytest.fixture(scope="module")
def deleg(localnet, account):
    return deploy_contract(localnet, account, "DelegationTest")


@pytest.fixture(scope="module")
def account2(localnet):
    acct = localnet.account.random()
    # Fund account2
    from conftest import fund_account
    dispenser = localnet.account.localnet_dispenser()
    fund_account(localnet, dispenser, acct)
    return acct


def test_deploy(deleg):
    assert deleg.app_id > 0


def test_admin(deleg, account):
    result = deleg.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_init_and_register(deleg, account):
    boxes = user_boxes(account.address)
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="initUser",
            args=[account.address],
            box_references=boxes,
        )
    )
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="register",
            args=[account.address, 100],
            box_references=boxes,
        )
    )


def test_is_registered(deleg, account):
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="isRegistered",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=registered_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_power(deleg, account):
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getPower",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=power_key(account.address))],
        )
    )
    assert result.abi_return == 100


def test_user_count(deleg):
    result = deleg.send.call(
        au.AppClientMethodCallParams(method="getUserCount")
    )
    assert result.abi_return == 1


def test_total_power(deleg):
    result = deleg.send.call(
        au.AppClientMethodCallParams(method="getTotalPower")
    )
    assert result.abi_return == 100


def test_effective_power_no_delegation(deleg, account):
    # Not delegating → effective = power + delegatedPower = 100 + 0
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getEffectivePower",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=power_key(account.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account.address)),
                au.BoxReference(app_id=0, name=delegate_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 100


def test_register_account2(deleg, account2):
    boxes = user_boxes(account2.address)
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="initUser",
            args=[account2.address],
            box_references=boxes,
        )
    )
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="register",
            args=[account2.address, 50],
            box_references=boxes,
            note=b"r2",
        )
    )


def test_delegate(deleg, account, account2):
    # account delegates to account2
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="delegate",
            args=[account.address, account2.address],
            box_references=[
                au.BoxReference(app_id=0, name=registered_key(account.address)),
                au.BoxReference(app_id=0, name=registered_key(account2.address)),
                au.BoxReference(app_id=0, name=delegate_key(account.address)),
                au.BoxReference(app_id=0, name=power_key(account.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account2.address)),
            ],
        )
    )


def test_delegate_to(deleg, account, account2):
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getDelegateTo",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=delegate_key(account.address))],
        )
    )
    assert result.abi_return == account2.address


def test_delegated_power(deleg, account2):
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getDelegatedPower",
            args=[account2.address],
            box_references=[au.BoxReference(app_id=0, name=delegated_power_key(account2.address))],
        )
    )
    assert result.abi_return == 100


def test_effective_power_delegator(deleg, account):
    # account is delegating → effective = 0 + delegatedPower(0) = 0
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getEffectivePower",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=power_key(account.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account.address)),
                au.BoxReference(app_id=0, name=delegate_key(account.address)),
            ],
            note=b"ep2",
        )
    )
    assert result.abi_return == 0


def test_effective_power_delegate(deleg, account2):
    # account2 not delegating → effective = 50 + 100 = 150
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getEffectivePower",
            args=[account2.address],
            box_references=[
                au.BoxReference(app_id=0, name=power_key(account2.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account2.address)),
                au.BoxReference(app_id=0, name=delegate_key(account2.address)),
            ],
            note=b"ep3",
        )
    )
    assert result.abi_return == 150


def test_undelegate(deleg, account, account2):
    deleg.send.call(
        au.AppClientMethodCallParams(
            method="undelegate",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=delegate_key(account.address)),
                au.BoxReference(app_id=0, name=power_key(account.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account2.address)),
            ],
        )
    )


def test_effective_after_undelegate(deleg, account):
    result = deleg.send.call(
        au.AppClientMethodCallParams(
            method="getEffectivePower",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=power_key(account.address)),
                au.BoxReference(app_id=0, name=delegated_power_key(account.address)),
                au.BoxReference(app_id=0, name=delegate_key(account.address)),
            ],
            note=b"ep4",
        )
    )
    assert result.abi_return == 100


def test_total_power_final(deleg):
    result = deleg.send.call(
        au.AppClientMethodCallParams(method="getTotalPower", note=b"tp2")
    )
    assert result.abi_return == 150
