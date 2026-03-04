"""
Solmate ERC20 — Test Suite
Source: https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC20.sol
Tests from: https://github.com/transmissions11/solmate/blob/main/src/test/ERC20.t.sol

Tests translated from Foundry (Solidity) to Python/pytest for Algorand localnet.
Covers: mint, burn, approve, transfer, transferFrom, totalSupply tracking,
        infinite approval behavior.

EIP-2612 permit tests are excluded (requires abi.encode, ecrecover not fully
supported on AVM). Core ERC20 logic tests are faithfully translated.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


@pytest.fixture(scope="module")
def user1(localnet: au.AlgorandClient) -> SigningAccount:
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def erc20_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy Solmate ERC20."""
    client = deploy_contract(
        localnet, account, "ERC20",
        fund_amount=1_000_000,
    )
    return client


def _balance_box(owner: str) -> bytes:
    return mapping_box_key("_balanceOf", addr_bytes(owner))


def _allowance_box(owner: str, spender: str) -> bytes:
    return mapping_box_key("_allowance", addr_bytes(owner) + addr_bytes(spender))


# ─── Mint Tests (from testMint) ───

@pytest.mark.localnet
def test_mint(
    erc20_client: au.AppClient, account: SigningAccount
) -> None:
    """Mint should increase totalSupply and balanceOf."""
    app_id = erc20_client.app_id
    bal_box = _balance_box(account.address)

    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1000],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 1000

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 1000


# ─── Burn Tests (from testBurn) ───

@pytest.mark.localnet
def test_burn(
    erc20_client: au.AppClient, account: SigningAccount
) -> None:
    """Burn should decrease totalSupply and balanceOf."""
    app_id = erc20_client.app_id
    bal_box = _balance_box(account.address)

    # After test_mint: balance = 1000, totalSupply = 1000
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 300],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 700

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 700


# ─── Approve Tests (from testApprove) ───

@pytest.mark.localnet
def test_approve(
    erc20_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Approve should set allowance."""
    app_id = erc20_client.app_id
    allow_box = _allowance_box(account.address, user1.address)

    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 500],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 500


# ─── Transfer Tests (from testTransfer) ───

@pytest.mark.localnet
def test_transfer(
    erc20_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Transfer should update both balances, totalSupply unchanged."""
    app_id = erc20_client.app_id
    sender_bal = _balance_box(account.address)
    recv_bal = _balance_box(user1.address)

    # Balance after mint(1000) + burn(300) = 700
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[user1.address, 200],
            box_references=[
                box_ref(app_id, sender_bal),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 500  # 700 - 200

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 200

    # totalSupply should be unchanged
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 700


# ─── TransferFrom Tests (from testTransferFrom) ───

@pytest.mark.localnet
def test_transfer_from(
    erc20_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """TransferFrom should work with approval and deduct allowance."""
    app_id = erc20_client.app_id
    sender_bal = _balance_box(account.address)
    recv_bal = _balance_box(user1.address)
    allow_box = _allowance_box(account.address, user1.address)

    # Set allowance to 300 (resets previous approval from test_approve)
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 300],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 calls transferFrom: account → user1, 150
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc20_client.app_spec,
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
                box_ref(app_id, sender_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance reduced
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 150  # 300 - 150

    # account balance: 500 - 150 = 350
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 350

    # user1 balance: 200 + 150 = 350
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 350


# ─── Infinite Approval TransferFrom (from testInfiniteApproveTransferFrom) ───

@pytest.mark.localnet
def test_infinite_approve_transfer_from(
    erc20_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """type(uint256).max approval should not decrease on transferFrom."""
    app_id = erc20_client.app_id
    MAX_UINT = 2**256 - 1
    sender_bal = _balance_box(account.address)
    recv_bal = _balance_box(user1.address)
    allow_box = _allowance_box(account.address, user1.address)

    # Set infinite approval
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, MAX_UINT],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 transfers 50
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc20_client.app_spec,
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
                box_ref(app_id, sender_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance still max
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return >= MAX_UINT


# ─── Total Supply Tracking ───

@pytest.mark.localnet
def test_total_supply_after_operations(
    erc20_client: au.AppClient,
) -> None:
    """totalSupply should reflect all mints and burns."""
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    # 1000 minted - 300 burned = 700
    assert result.abi_return == 700


# ─── Multiple Mints ───

@pytest.mark.localnet
def test_mint_to_different_users(
    erc20_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Minting to different users should update each balance independently."""
    app_id = erc20_client.app_id
    user1_bal = _balance_box(user1.address)

    # Mint additional 100 to user1
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user1.address, 100],
            box_references=[box_ref(app_id, user1_bal)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_bal)],
        )
    )
    # user1 had: 200 (transfer) + 150 (transferFrom) + 50 (infinite) + 100 (this mint) = 500
    assert result.abi_return == 500

    # totalSupply: 700 + 100 = 800
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == 800


# ─── Approve Overwrites Previous ───

@pytest.mark.localnet
def test_approve_overwrites(
    erc20_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """New approve should overwrite previous allowance."""
    app_id = erc20_client.app_id
    allow_box = _allowance_box(account.address, user1.address)

    # Set to 100 (overwrites max from infinite test)
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 100],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 100

    # Set to 0
    erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 0],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 0
