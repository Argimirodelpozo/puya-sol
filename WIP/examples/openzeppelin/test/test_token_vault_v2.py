"""
TokenVaultV2 behavioral tests.
Tests deposit/withdrawal with fee calculation, owner access control,
compound assignments on multiple mappings, and tuple returns.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def vault(localnet, account):
    return deploy_contract(localnet, account, "TokenVaultV2")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


# --- Deploy ---

def test_deploy(vault):
    assert vault.app_id > 0


def test_owner(vault, account):
    result = vault.send.call(au.AppClientMethodCallParams(method="owner"))
    assert result.abi_return == account.address


def test_initial_fee(vault):
    result = vault.send.call(au.AppClientMethodCallParams(method="withdrawalFeeBps"))
    assert result.abi_return == 200  # 2% default fee


# --- Deposit ---

def test_deposit(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    cnt_key = mapping_box_key("_depositCount", addr)
    dep_key = mapping_box_key("_totalDeposited", addr)

    vault.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[1000],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, cnt_key),
                box_ref(app_id, dep_key),
            ],
        )
    )


def test_balance_after_deposit(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(app_id, bal_key)],
        )
    )
    assert result.abi_return == 1000


def test_deposit_count(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    cnt_key = mapping_box_key("_depositCount", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="depositCount",
            args=[addr],
            box_references=[box_ref(app_id, cnt_key)],
        )
    )
    assert result.abi_return == 1


def test_total_balance(vault):
    result = vault.send.call(au.AppClientMethodCallParams(method="totalBalance"))
    assert result.abi_return == 1000


def test_depositors_count(vault):
    result = vault.send.call(au.AppClientMethodCallParams(method="depositorsCount"))
    assert result.abi_return == 1


# --- Second deposit ---

def test_second_deposit(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    cnt_key = mapping_box_key("_depositCount", addr)
    dep_key = mapping_box_key("_totalDeposited", addr)

    vault.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[500],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, cnt_key),
                box_ref(app_id, dep_key),
            ],
        )
    )


def test_balance_after_two_deposits(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(app_id, bal_key)],
        )
    )
    assert result.abi_return == 1500


def test_total_deposited(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    dep_key = mapping_box_key("_totalDeposited", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="totalDeposited",
            args=[addr],
            box_references=[box_ref(app_id, dep_key)],
        )
    )
    assert result.abi_return == 1500


def test_deposit_count_two(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    cnt_key = mapping_box_key("_depositCount", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="depositCount",
            args=[addr],
            box_references=[box_ref(app_id, cnt_key)],
        )
    )
    assert result.abi_return == 2


# --- Withdraw ---

def test_withdraw(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    wd_key = mapping_box_key("_totalWithdrawn", addr)

    vault.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[500],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, wd_key),
            ],
        )
    )


def test_balance_after_withdraw(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(app_id, bal_key)],
        )
    )
    assert result.abi_return == 1000  # 1500 - 500


def test_total_balance_after_withdraw(vault):
    result = vault.send.call(au.AppClientMethodCallParams(method="totalBalance"))
    assert result.abi_return == 1000


# --- Calculate fee ---

def test_calculate_fee(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="calculateFee",
            args=[10000],
        )
    )
    # 2% of 10000 = 200
    assert result.abi_return == 200


# --- Insufficient balance ---

def test_withdraw_insufficient_fails(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    wd_key = mapping_box_key("_totalWithdrawn", addr)

    with pytest.raises(Exception):
        vault.send.call(
            au.AppClientMethodCallParams(
                method="withdraw",
                args=[99999],
                box_references=[
                    box_ref(app_id, bal_key),
                    box_ref(app_id, wd_key),
                ],
            )
        )


# --- Zero amount ---

def test_deposit_zero_fails(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    cnt_key = mapping_box_key("_depositCount", addr)
    dep_key = mapping_box_key("_totalDeposited", addr)

    with pytest.raises(Exception):
        vault.send.call(
            au.AppClientMethodCallParams(
                method="deposit",
                args=[0],
                box_references=[
                    box_ref(app_id, bal_key),
                    box_ref(app_id, cnt_key),
                    box_ref(app_id, dep_key),
                ],
            )
        )


# --- Account info tuple return ---

def test_account_info(vault, account):
    app_id = vault.app_id
    addr = addr_bytes(account.address)
    bal_key = mapping_box_key("_balances", addr)
    cnt_key = mapping_box_key("_depositCount", addr)
    dep_key = mapping_box_key("_totalDeposited", addr)
    wd_key = mapping_box_key("_totalWithdrawn", addr)

    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="accountInfo",
            args=[addr],
            box_references=[
                box_ref(app_id, bal_key),
                box_ref(app_id, cnt_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, wd_key),
            ],
        )
    )
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["balance"] == 1000
        assert ret["deposits"] == 2
        assert ret["deposited"] == 1500
    else:
        assert ret[0] == 1000  # balance
        assert ret[1] == 2     # deposits
        assert ret[2] == 1500  # total deposited


# --- Non-owner cannot set fee ---

def test_non_owner_cannot_set_fee(vault, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=vault.app_spec,
            app_id=vault.app_id,
            default_sender=account2.address,
        )
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="setFee",
                args=[500],
            )
        )
