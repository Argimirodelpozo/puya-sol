"""
Solmate ERC6909 — Test Suite
Source: https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC6909.sol
Tests from: https://github.com/transmissions11/solmate/blob/main/src/test/ERC6909.t.sol

Tests translated from Foundry (Solidity) to Python/pytest for Algorand localnet.
Covers: mint, burn, transfer, transferFrom, approve, setOperator, operator bypass,
        infinite approval, triple-nested mapping (allowance), balanceOf, isOperator.

ERC6909 is a minimalist multi-token standard. Each (owner, id) pair has a balance.
Allowances are per (owner, spender, id) — a triple-nested mapping.
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
    """Used for approval-based tests (not operator)."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def user2(localnet: au.AlgorandClient) -> SigningAccount:
    """Used for operator-based tests."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def erc6909_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy ERC6909."""
    client = deploy_contract(
        localnet, account, "ERC6909",
        fund_amount=2_000_000,
    )
    return client


def _balance_box(owner: str, token_id: int) -> bytes:
    return mapping_box_key("_balanceOf", addr_bytes(owner) + id_bytes(token_id))


def _allowance_box(owner: str, spender: str, token_id: int) -> bytes:
    return mapping_box_key("_allowance", addr_bytes(owner) + addr_bytes(spender) + id_bytes(token_id))


def _operator_box(owner: str, operator: str) -> bytes:
    return mapping_box_key("_isOperator", addr_bytes(owner) + addr_bytes(operator))


# ─── Mint Tests (from testMint) ───

@pytest.mark.localnet
def test_mint(
    erc6909_client: au.AppClient, account: SigningAccount
) -> None:
    """token.mint(address(0xBEEF), 1337, 100) → balanceOf == 100"""
    app_id = erc6909_client.app_id
    bal_box = _balance_box(account.address, 1337)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1337, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc6909_client.send.call(
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
    erc6909_client: au.AppClient, account: SigningAccount
) -> None:
    """mint 100, burn 70 → balanceOf == 30"""
    app_id = erc6909_client.app_id
    bal_box = _balance_box(account.address, 42)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 42, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 42, 70],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 42],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 30


# ─── Approve Tests (from testApprove) ───

@pytest.mark.localnet
def test_approve(
    erc6909_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """approve(user1, 1337, 100) → allowance(account, user1, 1337) == 100"""
    app_id = erc6909_client.app_id
    allow_box = _allowance_box(account.address, user1.address, 1337)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 1337, 100],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address, 1337],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 100


# ─── Transfer Tests (from testTransfer) ───

@pytest.mark.localnet
def test_transfer(
    erc6909_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """mint 100 to account, transfer 70 to user1 → account=30, user1=70"""
    app_id = erc6909_client.app_id
    token_id = 99
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user1.address, token_id)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[user1.address, token_id, 70],
            box_references=[
                box_ref(app_id, sender_bal),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 30

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 70


# ─── TransferFrom with Approval (from testTransferFromWithApproval) ───
# Uses user1 who is NOT an operator, so the allowance path is exercised

@pytest.mark.localnet
def test_transfer_from_with_approval(
    erc6909_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """mint → approve → transferFrom → allowance reduced, balances updated"""
    app_id = erc6909_client.app_id
    token_id = 200
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user1.address, token_id)
    allow_box = _allowance_box(account.address, user1.address, token_id)
    op_box = _operator_box(account.address, user1.address)

    # Mint 100 to account
    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    # Account approves user1 for 100 on token_id
    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, token_id, 100],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 calls transferFrom (account → user1, 70)
    # Need operator box ref since transferFrom checks isOperator first
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc6909_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, token_id, 70],
            box_references=[
                box_ref(app_id, sender_bal),
                box_ref(app_id, op_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance reduced to 30
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address, token_id],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 30

    # Sender balance = 30
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 30

    # Receiver balance = 70
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 70


# ─── TransferFrom with Infinite Approval (from testTransferFromWithInfiniteApproval) ───

@pytest.mark.localnet
def test_transfer_from_infinite_approval(
    erc6909_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Infinite approval should NOT decrease on transferFrom."""
    app_id = erc6909_client.app_id
    token_id = 300
    MAX_UINT = 2**256 - 1
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user1.address, token_id)
    allow_box = _allowance_box(account.address, user1.address, token_id)
    op_box = _operator_box(account.address, user1.address)

    # Mint 100 to account
    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    # Set infinite approval
    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, token_id, MAX_UINT],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 transfers 70
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc6909_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, token_id, 70],
            box_references=[
                box_ref(app_id, sender_bal),
                box_ref(app_id, op_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance should still be max
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, user1.address, token_id],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return >= MAX_UINT

    # Balances
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 30

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user1.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 70


# ─── SetOperator Tests (from testSetOperator) ───
# Uses user2 to avoid operator state affecting approval tests

@pytest.mark.localnet
def test_set_operator(
    erc6909_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """setOperator(user2, true) → isOperator(account, user2) == true"""
    app_id = erc6909_client.app_id
    op_box = _operator_box(account.address, user2.address)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="setOperator",
            args=[user2.address, True],
            box_references=[box_ref(app_id, op_box)],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="isOperator",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, op_box)],
        )
    )
    assert result.abi_return is True


# ─── TransferFrom as Operator (from testTransferFromAsOperator) ───
# Uses user2 who IS an operator (set in test_set_operator)

@pytest.mark.localnet
def test_transfer_from_as_operator(
    erc6909_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Operator can transferFrom without allowance."""
    app_id = erc6909_client.app_id
    token_id = 400
    sender_bal = _balance_box(account.address, token_id)
    recv_bal = _balance_box(user2.address, token_id)
    op_box = _operator_box(account.address, user2.address)

    # Mint 100 to account
    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )

    # user2 is already operator from test_set_operator
    # user2 transfers 70 from account (as operator, no allowance needed)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc6909_client.app_spec,
            app_id=app_id,
            default_sender=user2.address,
        )
    )
    localnet.account.set_signer_from_account(user2)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user2.address, token_id, 70],
            box_references=[
                box_ref(app_id, sender_bal),
                box_ref(app_id, op_box),
                box_ref(app_id, recv_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Check balances
    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, sender_bal)],
        )
    )
    assert result.abi_return == 30

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[user2.address, token_id],
            box_references=[box_ref(app_id, recv_bal)],
        )
    )
    assert result.abi_return == 70


# ─── Multiple Token IDs ───

@pytest.mark.localnet
def test_multiple_token_ids(
    erc6909_client: au.AppClient, account: SigningAccount
) -> None:
    """Multiple token IDs should have independent balances."""
    app_id = erc6909_client.app_id
    bal_id1 = _balance_box(account.address, 1)
    bal_id2 = _balance_box(account.address, 2)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1, 500],
            box_references=[box_ref(app_id, bal_id1)],
        )
    )

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 2, 1000],
            box_references=[box_ref(app_id, bal_id2)],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(app_id, bal_id1)],
        )
    )
    assert result.abi_return == 500

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 2],
            box_references=[box_ref(app_id, bal_id2)],
        )
    )
    assert result.abi_return == 1000


# ─── Self-Transfer (from fuzz test edge case: sender == receiver) ───

@pytest.mark.localnet
def test_self_transfer(
    erc6909_client: au.AppClient, account: SigningAccount
) -> None:
    """Transferring to self should maintain balance."""
    app_id = erc6909_client.app_id
    token_id = 555
    bal_box = _balance_box(account.address, token_id)

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, token_id, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[account.address, token_id, 50],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, bal_box),
            ],
        )
    )

    result = erc6909_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, token_id],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == 100
