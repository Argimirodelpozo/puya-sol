"""
WETH9 (Wrapped Ether) — Test Suite
Source: https://github.com/gnosis/canonical-weth/blob/master/contracts/WETH9.sol

Tests based on standard WETH9 behavior (no tests in original repo).
Covers: deposit, withdraw, transfer, transferFrom, approve, allowance,
totalSupply tracking, infinite approval behavior.

WETH9 is deployed on every EVM chain. This tests the ERC20 logic which
is identical to the original. Deposit/withdraw adapted to use explicit
amounts instead of msg.value (see contract modifications).
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def pad64(value: int) -> bytes:
    return value.to_bytes(64, "big")


@pytest.fixture(scope="module")
def user1(localnet: au.AlgorandClient) -> SigningAccount:
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def weth_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy WETH9 and deposit some initial tokens."""
    client = deploy_contract(
        localnet, account, "WETH9",
        fund_amount=1_000_000,
    )
    app_id = client.app_id

    # Deposit 1000 WETH for admin
    bal_box = mapping_box_key("balanceOf", addr_bytes(account.address))
    client.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[1000],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    return client


# ─── Deposit Tests ───

@pytest.mark.localnet
def test_deposit_updates_balance(
    weth_client: au.AppClient, account: SigningAccount
) -> None:
    """Deposit should credit balanceOf."""
    app_id = weth_client.app_id
    bal_box = mapping_box_key("balanceOf", addr_bytes(account.address))

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 1000


@pytest.mark.localnet
def test_deposit_updates_total_supply(weth_client: au.AppClient) -> None:
    """totalSupply should reflect total deposits."""
    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 1000


# ─── Withdraw Tests ───

@pytest.mark.localnet
def test_withdraw(
    weth_client: au.AppClient, account: SigningAccount
) -> None:
    """Withdraw should debit balanceOf and totalSupply."""
    app_id = weth_client.app_id
    bal_box = mapping_box_key("balanceOf", addr_bytes(account.address))

    weth_client.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 900

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 900


# ─── Transfer Tests ───

@pytest.mark.localnet
def test_transfer(
    weth_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """transfer() should move tokens between accounts."""
    app_id = weth_client.app_id
    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal = mapping_box_key("balanceOf", addr_bytes(user1.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    weth_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[user1.address, 200],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user1_bal),
            ],
        )
    )

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_bal)],
        )
    )
    assert result.abi_return == 200

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    assert result.abi_return == 700  # 900 - 200


# ─── Approve + TransferFrom Tests ───

@pytest.mark.localnet
def test_approve_and_transfer_from(
    weth_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """approve + transferFrom should work with allowance deduction."""
    app_id = weth_client.app_id
    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal = mapping_box_key("balanceOf", addr_bytes(user1.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user1.address)
    )

    # Admin approves user1 for 300
    weth_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 300],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 300

    # user1 transfers 150 from admin to self
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=weth_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, 150],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user1_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance reduced to 150
    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 150

    # user1 balance increased by 150
    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_bal)],
        )
    )
    assert result.abi_return == 350  # 200 + 150


# ─── Infinite Approval (uint256.max) ───

@pytest.mark.localnet
def test_infinite_approval(
    weth_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Infinite approval should not decrease on transferFrom."""
    app_id = weth_client.app_id
    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal = mapping_box_key("balanceOf", addr_bytes(user1.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user1.address)
    )
    MAX_UINT = 2**256 - 1

    # Set infinite approval
    weth_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, MAX_UINT],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 transfers 50 from admin
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=weth_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, 50],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user1_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance should still be max (infinite approval not reduced)
    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return >= MAX_UINT


# ─── Total Supply Consistency ───

@pytest.mark.localnet
def test_total_supply_consistent(weth_client: au.AppClient) -> None:
    """Total supply should reflect all deposits minus withdrawals."""
    result = weth_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    # 1000 deposited - 100 withdrawn = 900
    assert result.abi_return == 900
