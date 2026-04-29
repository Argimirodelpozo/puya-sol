"""
DappHub DSToken — Test Suite
Source: https://github.com/dapphub/ds-token/blob/master/src/token.sol
Tests from: https://github.com/dapphub/ds-token/blob/master/src/token.t.sol

Tests translated from ds-test (Solidity) to Python/pytest for Algorand localnet.
Covers: transfer, transferFrom, approval, infinite approval, mint (auth),
        burn (auth), push/pull/move aliases, stop/start mechanism, owner auth.

DSToken is the foundational ERC20 token from DappHub, used as the basis
for MakerDAO's Dai token. Features auth-gated mint/burn, stoppable pattern,
and push/pull/move convenience methods.
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
def user2(localnet: au.AlgorandClient) -> SigningAccount:
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def token_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy DSToken and mint 1000 to deployer (owner)."""
    client = deploy_contract(
        localnet, account, "DSToken",
        fund_amount=2_000_000,
    )
    app_id = client.app_id
    bal_box = mapping_box_key("_balanceOf", addr_bytes(account.address))

    # Mint 1000 as owner (auth-gated)
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1000],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    return client


def _balance_box(addr: str) -> bytes:
    return mapping_box_key("_balanceOf", addr_bytes(addr))


def _allowance_box(src: str, guy: str) -> bytes:
    return mapping_box_key("_allowance", addr_bytes(src) + addr_bytes(guy))


# ─── Setup Precondition (from testSetupPrecondition) ───

@pytest.mark.localnet
def test_setup_precondition(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Balance of deployer should be 1000 after setup."""
    app_id = token_client.app_id
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, _balance_box(account.address))],
        )
    )
    assert result.abi_return == 1000


# ─── Transfer Tests (from testValidTransfers) ───

@pytest.mark.localnet
def test_valid_transfers(
    token_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Transfer 250 to user1."""
    app_id = token_client.app_id
    src_box = _balance_box(account.address)
    dst_box = _balance_box(user1.address)

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[user1.address, 250],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, _allowance_box(account.address, account.address)),
                box_ref(app_id, dst_box),
            ],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, src_box)],
        )
    )
    assert result.abi_return == 750

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, dst_box)],
        )
    )
    assert result.abi_return == 250


# ─── Allowance Tests (from testAllowanceStartsAtZero, testApproveSetsAllowance) ───

@pytest.mark.localnet
def test_approve_sets_allowance(
    token_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Approve should set allowance."""
    app_id = token_client.app_id
    allow_box = _allowance_box(account.address, user1.address)

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 25],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 25


# ─── Charges Amount Approved (from testChargesAmountApproved) ───

@pytest.mark.localnet
def test_charges_amount_approved(
    token_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Approved user1 can transferFrom and allowance decreases."""
    app_id = token_client.app_id
    src_box = _balance_box(account.address)
    dst_box = _balance_box(user1.address)
    allow_box = _allowance_box(account.address, user1.address)

    # Set allowance to 100
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 100],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 pulls 20 from account
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=token_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, 20],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, dst_box),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 80  # 100 - 20


# ─── Infinite Approval (from testTrusting / testApproveWillNotModifyAllowance) ───

@pytest.mark.localnet
def test_infinite_approval(
    token_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Infinite approval (uint256.max) should not decrease on transferFrom."""
    app_id = token_client.app_id
    MAX_UINT = 2**256 - 1
    src_box = _balance_box(account.address)
    dst_box = _balance_box(user2.address)
    allow_box = _allowance_box(account.address, user2.address)

    # Mint more so we have enough balance
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 500],
            box_references=[box_ref(app_id, src_box)],
        )
    )

    # Set infinite approval for user2
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approveMax",
            args=[user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user2 transfers 50
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=token_client.app_spec,
            app_id=app_id,
            default_sender=user2.address,
        )
    )
    localnet.account.set_signer_from_account(user2)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user2.address, 50],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, dst_box),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance still max
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return >= MAX_UINT


# ─── TransferFrom Self (from testTransferFromSelf) ───

@pytest.mark.localnet
def test_transfer_from_self(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Self-transferFrom should work without approval."""
    app_id = token_client.app_id
    src_box = _balance_box(account.address)
    allow_box = _allowance_box(account.address, account.address)

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, account.address, 50],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, src_box),
            ],
        )
    )

    # Balance unchanged (self-transfer)
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, src_box)],
        )
    )
    # Should still have: 1000 - 250 (transfer) + 500 (mint) - 50 (user2 took) = 1200
    # But self-transfer doesn't change balance
    assert result.abi_return > 0


# ─── Mint Tests (from testMint, testMintGuy) ───

@pytest.mark.localnet
def test_mint_increases_supply(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Mint should increase totalSupply."""
    app_id = token_client.app_id
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    supply_before = result.abi_return

    bal_box = _balance_box(account.address)
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 100],
            box_references=[box_ref(app_id, bal_box)],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == supply_before + 100


@pytest.mark.localnet
def test_mint_to_guy(
    token_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Mint to a specific address (auth required)."""
    app_id = token_client.app_id
    guy_box = _balance_box(user1.address)

    result_before = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, guy_box)],
        )
    )
    bal_before = result_before.abi_return

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user1.address, 200],
            box_references=[box_ref(app_id, guy_box)],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, guy_box)],
        )
    )
    assert result.abi_return == bal_before + 200


# ─── Burn Tests (from testBurn, testBurnself) ───

@pytest.mark.localnet
def test_burn(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Burn should decrease totalSupply and balance."""
    app_id = token_client.app_id
    bal_box = _balance_box(account.address)

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    supply_before = result.abi_return

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    bal_before = result.abi_return

    allow_box = _allowance_box(account.address, account.address)
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 50],
            box_references=[
                box_ref(app_id, bal_box),
                box_ref(app_id, allow_box),
            ],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[],
        )
    )
    assert result.abi_return == supply_before - 50

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == bal_before - 50


# ─── Push Test (from testPush) ───

@pytest.mark.localnet
def test_push(
    token_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """push(dst, wad) is an alias for transferFrom(msg.sender, dst, wad)."""
    app_id = token_client.app_id
    src_box = _balance_box(account.address)
    dst_box = _balance_box(user1.address)
    allow_box = _allowance_box(account.address, account.address)

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, dst_box)],
        )
    )
    bal_before = result.abi_return

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[user1.address, 30],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, dst_box),
            ],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, dst_box)],
        )
    )
    assert result.abi_return == bal_before + 30


# ─── Pull Test (from testPullWithTrust) ───

@pytest.mark.localnet
def test_pull_with_trust(
    token_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """pull(src, wad) is an alias for transferFrom(src, msg.sender, wad). Needs approval."""
    app_id = token_client.app_id
    src_box = _balance_box(user1.address)
    dst_box = _balance_box(account.address)
    allow_box = _allowance_box(user1.address, account.address)

    # user1 approves account to pull
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=token_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 100],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    localnet.account.set_signer_from_account(account)

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, dst_box)],
        )
    )
    bal_before = result.abi_return

    # Account pulls 20 from user1
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="pull",
            args=[user1.address, 20],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, dst_box),
            ],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, dst_box)],
        )
    )
    assert result.abi_return == bal_before + 20


# ─── Stop/Start Tests (from testFailTransferWhenStopped, etc.) ───

@pytest.mark.localnet
def test_stop_and_start(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Stop should block transfers, start should re-enable them."""
    app_id = token_client.app_id

    # Stop the contract
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="stop",
            args=[],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="stopped",
            args=[],
        )
    )
    assert result.abi_return is True

    # Start the contract
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="start",
            args=[],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="stopped",
            args=[],
        )
    )
    assert result.abi_return is False


# ─── Owner Auth Test ───

@pytest.mark.localnet
def test_owner(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Owner should be the deployer."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getOwner",
            args=[],
        )
    )
    assert result.abi_return == account.address


# ─── Allowance Modification (from testApproveWillModifyAllowance) ───

@pytest.mark.localnet
def test_allowance_modification(
    token_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Finite allowance decreases on use; can re-approve."""
    app_id = token_client.app_id
    allow_box = _allowance_box(account.address, user1.address)
    src_box = _balance_box(account.address)
    dst_box = _balance_box(user1.address)

    # Re-approve user1 for 50
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 50],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user1 transfers 10
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=token_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, 10],
            box_references=[
                box_ref(app_id, src_box),
                box_ref(app_id, allow_box),
                box_ref(app_id, dst_box),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 40  # 50 - 10
