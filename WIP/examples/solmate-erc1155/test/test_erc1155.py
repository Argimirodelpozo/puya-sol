"""
Solmate ERC1155 — Test Suite
Source: https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC1155.sol
Tests from: https://github.com/transmissions11/solmate/blob/main/src/test/ERC1155.t.sol

Tests translated from Foundry (Solidity) to Python/pytest for Algorand localnet.
Covers: mint, burn, safeTransferFrom, setApprovalForAll, operator bypass,
        multiple token IDs, self-transfer, balanceOf, isApprovedForAll.

Modifications for AVM: removed batch operations (dynamic array params),
removed safe transfer callbacks (cross-contract), removed uri/supportsInterface,
made concrete with public mint/burn, added explicit getter functions.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def id_bytes(token_id: int) -> bytes:
    """Encode a uint256 token ID as 64-byte biguint for mapping keys."""
    return token_id.to_bytes(64, "big")


@pytest.fixture(scope="module")
def user1(localnet: au.AlgorandClient) -> SigningAccount:
    """Standard user for transfer tests."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def user2(localnet: au.AlgorandClient) -> SigningAccount:
    """Operator user for approval tests."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def erc1155_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy ERC1155."""
    client = deploy_contract(
        localnet, account, "ERC1155",
        fund_amount=2_000_000,
    )
    return client


def _balance_box(owner: str, token_id: int) -> bytes:
    return mapping_box_key("_balanceOf", addr_bytes(owner) + id_bytes(token_id))


def _approval_box(owner: str, operator: str) -> bytes:
    return mapping_box_key("_isApprovedForAll", addr_bytes(owner) + addr_bytes(operator))


# ─── Mint Tests (from testMintToEOA) ───

@pytest.mark.localnet
def test_mint(
    erc1155_client: au.AppClient, account: SigningAccount
) -> None:
    """token.mint(account, 1337, 100) → balanceOf(account, 1337) == 100"""
    app_id = erc1155_client.app_id
    bal_box = _balance_box(account.address, 1337)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1337, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1337],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 100


# ─── Burn Tests (from testBurn) ───

@pytest.mark.localnet
def test_burn(
    erc1155_client: au.AppClient, account: SigningAccount
) -> None:
    """mint 100, burn 70 → balanceOf == 30"""
    app_id = erc1155_client.app_id
    bal_box = _balance_box(account.address, 42)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 42, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 42, 70],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 42],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 30


# ─── Approve All Tests (from testApproveAll) ───

@pytest.mark.localnet
def test_approve_all(
    erc1155_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """setApprovalForAll(user2, true) → isApprovedForAll(account, user2) == true"""
    app_id = erc1155_client.app_id
    appr_box = _approval_box(account.address, user2.address)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[user2.address, True],
            box_references=[box_ref(app_id, appr_box)],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="isApprovedForAll",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, appr_box)],
        )
    )
    assert result.abi_return is True


# ─── Safe Transfer From (from testSafeTransferFromToEOA) ───

@pytest.mark.localnet
def test_safe_transfer_from(
    erc1155_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """mint 100, safeTransferFrom 70 to user1 → account=30, user1=70"""
    app_id = erc1155_client.app_id
    token_id = 99
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user1.address, token_id)
    appr_box = _approval_box(account.address, account.address)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[account.address, user1.address, token_id, 70],
            box_references=[
                box_ref(app_id, appr_box),
                box_ref(app_id, sender_bal),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 30

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 70


# ─── Safe Transfer From Self (from testSafeTransferFromSelf) ───

@pytest.mark.localnet
def test_safe_transfer_from_self(
    erc1155_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Self-transfer: msg.sender == from, no approval needed."""
    app_id = erc1155_client.app_id
    token_id = 101
    bal_box = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user1.address, token_id)
    appr_box = _approval_box(account.address, account.address)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    # Transfer from self (msg.sender == from)
    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[account.address, user1.address, token_id, 60],
            box_references=[
                box_ref(app_id, appr_box),
                box_ref(app_id, bal_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 40


# ─── Transfer via Approved Operator ───

@pytest.mark.localnet
def test_transfer_from_approved_operator(
    erc1155_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Approved operator (from test_approve_all) can transfer without specific approval."""
    app_id = erc1155_client.app_id
    token_id = 500
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user2.address, token_id)
    appr_box = _approval_box(account.address, user2.address)

    # Mint 100 to account
    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    # user2 is already operator from test_approve_all
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc1155_client.app_spec,
            app_id=app_id,
            default_sender=user2.address,
        )
    )
    localnet.account.set_signer_from_account(user2)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[account.address, user2.address, token_id, 80],
            box_references=[
                box_ref(app_id, appr_box),
                box_ref(app_id, sender_bal),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Check balances
    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 20

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user2.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 80


# ─── Multiple Token IDs (from fuzz test edge cases) ───

@pytest.mark.localnet
def test_multiple_token_ids(
    erc1155_client: au.AppClient, account: SigningAccount
) -> None:
    """Multiple token IDs should have independent balances."""
    app_id = erc1155_client.app_id
    bal_id1 = _balance_box(account.address, 1)
    bal_id2 = _balance_box(account.address, 2)
    bal_id3 = _balance_box(account.address, 3)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1, 100],
            box_references=[box_ref(app_id, bal_id1)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 2, 200],
            box_references=[box_ref(app_id, bal_id2)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 3, 300],
            box_references=[box_ref(app_id, bal_id3)],
        )
    )

    for tid, expected in [(1, 100), (2, 200), (3, 300)]:
        result = erc1155_client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf",
                args=[account.address, tid],
                box_references=[box_ref(app_id, _balance_box(account.address, tid))],
            )
        )
        assert result.abi_return == expected


# ─── Self Transfer (edge case from fuzz tests) ───

@pytest.mark.localnet
def test_self_transfer(
    erc1155_client: au.AppClient, account: SigningAccount
) -> None:
    """Transferring to self should maintain balance."""
    app_id = erc1155_client.app_id
    token_id = 777
    bal_box = _balance_box(account.address, token_id)
    appr_box = _approval_box(account.address, account.address)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[account.address, account.address, token_id, 50],
            box_references=[
                box_ref(app_id, appr_box),
                box_ref(app_id, bal_box),
                box_ref(app_id, bal_box),
            ],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 100


# ─── Revoke Approval (from testApproveAll with false) ───

@pytest.mark.localnet
def test_revoke_approval(
    erc1155_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """setApprovalForAll(user1, true) then setApprovalForAll(user1, false)."""
    app_id = erc1155_client.app_id
    appr_box = _approval_box(account.address, user1.address)

    # Set approval
    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[user1.address, True],
            box_references=[box_ref(app_id, appr_box)],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="isApprovedForAll",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, appr_box)],
        )
    )
    assert result.abi_return is True

    # Revoke approval
    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[user1.address, False],
            box_references=[box_ref(app_id, appr_box)],
        )
    )

    result = erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="isApprovedForAll",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, appr_box)],
        )
    )
    assert result.abi_return is False


# ─── Mint to Different Users (from fuzz test patterns) ───

@pytest.mark.localnet
def test_mint_to_different_users(
    erc1155_client: au.AppClient,
    account: SigningAccount,
    user1: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Minting same token ID to different users gives independent balances."""
    app_id = erc1155_client.app_id
    token_id = 888
    bal_acct = _balance_box(account.address, token_id)
    bal_user1 = _balance_box(user1.address, token_id)
    bal_user2 = _balance_box(user2.address, token_id)

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 50],
            box_references=[box_ref(app_id, bal_acct)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user1.address, token_id, 100],
            box_references=[box_ref(app_id, bal_user1)],
        )
    )

    erc1155_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user2.address, token_id, 200],
            box_references=[box_ref(app_id, bal_user2)],
        )
    )

    for addr, expected in [
        (account.address, 50),
        (user1.address, 100),
        (user2.address, 200),
    ]:
        result = erc1155_client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf",
                args=[addr, token_id],
                box_references=[box_ref(app_id, _balance_box(addr, token_id))],
            )
        )
        assert result.abi_return == expected
