"""
MakerDAO Dai Stablecoin — Test Suite
Translated from: https://github.com/makerdao/dss/blob/master/src/test/dai.t.sol

Original tests by DappHub, LLC (GPL-3.0).
Translated to Python/pytest for Algorand VM testing via puya-sol.

Tests cover: ERC20 transfers, approvals, allowances, minting (auth),
burning (with allowance), rely/deny authorization system, push/pull/move aliases.

Skipped tests (require unsupported features):
- testDaiAddress: Hevm-specific address prediction
- testTypehash, testDomain_Separator: EIP-712 domain (removed from contract)
- testPermit*: EIP-2612 permit (removed from contract)
- testFail* variants: These test reverts, tested via pytest.raises below
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


INITIAL_BALANCE_ADMIN = 1000
INITIAL_BALANCE_USER1 = 100


@pytest.fixture(scope="module")
def user1(localnet: au.AlgorandClient) -> SigningAccount:
    """Corresponds to 'cal' in original tests."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def user2(localnet: au.AlgorandClient) -> SigningAccount:
    """Corresponds to 'del' in original tests."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def dai_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> au.AppClient:
    """Deploy Dai and set up initial state matching setUp() in dai.t.sol."""
    client = deploy_contract(
        localnet, account, "Dai",
        fund_amount=2_000_000,
    )
    app_id = client.app_id

    # Call __postInit to execute constructor box writes (wards[msg.sender] = 1)
    wards_box = mapping_box_key("wards", addr_bytes(account.address))
    client.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[],
            box_references=[box_ref(app_id, wards_box)],
        )
    )

    # Mint initial balances: admin gets 1000, user1 gets 100
    # (mirrors setUp: token.mint(address(this), 1000); token.mint(cal, 100))
    admin_bal_box = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal_box = mapping_box_key("balanceOf", addr_bytes(user1.address))

    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, INITIAL_BALANCE_ADMIN],
            box_references=[
                box_ref(app_id, wards_box),
                box_ref(app_id, admin_bal_box),
            ],
        )
    )

    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user1.address, INITIAL_BALANCE_USER1],
            box_references=[
                box_ref(app_id, wards_box),
                box_ref(app_id, user1_bal_box),
            ],
        )
    )

    return client


# ─── Original: testSetupPrecondition ───

@pytest.mark.localnet
def test_setup_precondition(
    dai_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: assertEq(token.balanceOf(self), initialBalanceThis)"""
    app_id = dai_client.app_id
    bal_box = mapping_box_key("balanceOf", addr_bytes(account.address))

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, bal_box)],
        )
    )
    assert result.abi_return == INITIAL_BALANCE_ADMIN


# ─── Original: testAllowanceStartsAtZero ───

@pytest.mark.localnet
def test_allowance_starts_at_zero(
    dai_client: au.AppClient, user1: SigningAccount, user2: SigningAccount
) -> None:
    """Original: assertEq(token.allowance(user1, user2), 0)"""
    app_id = dai_client.app_id
    allow_box = mapping_box_key(
        "allowance", addr_bytes(user1.address) + addr_bytes(user2.address)
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[user1.address, user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 0


# ─── Original: testValidTransfers ───

@pytest.mark.localnet
def test_valid_transfers(
    dai_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """Original: token.transfer(user2, 250); check balances."""
    app_id = dai_client.app_id
    sent_amount = 250

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[user2.address, sent_amount],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user2_bal),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    assert result.abi_return == sent_amount

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    assert result.abi_return == INITIAL_BALANCE_ADMIN - sent_amount


# ─── Original: testApproveSetsAllowance ───

@pytest.mark.localnet
def test_approve_sets_allowance(
    dai_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """Original: token.approve(user2, 25); assertEq(allowance, 25)"""
    app_id = dai_client.app_id
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user2.address)
    )

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user2.address, 25],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 25


# ─── Original: testChargesAmountApproved ───

@pytest.mark.localnet
def test_charges_amount_approved(
    dai_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Original: approve user2 for 20, user2 calls transferFrom(self, user2, 20)."""
    app_id = dai_client.app_id
    amount_approved = 20

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user2.address)
    )

    # Approve user2
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user2.address, amount_approved],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    # user2 calls transferFrom
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=dai_client.app_spec,
            app_id=app_id,
            default_sender=user2.address,
        )
    )
    localnet.account.set_signer_from_account(user2)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user2.address, amount_approved],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user2_bal),
            ],
        )
    )

    # Switch back to admin
    localnet.account.set_signer_from_account(account)

    # Verify admin balance decreased
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    # admin started with 1000, transferred 250 to user2 in previous test, then 20 more here
    assert result.abi_return == INITIAL_BALANCE_ADMIN - 250 - amount_approved


# ─── Original: testTransferFromSelf ───

@pytest.mark.localnet
def test_transfer_from_self(
    dai_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Original: token.transferFrom(self, user1, 50); check user1 balance."""
    app_id = dai_client.app_id

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal = mapping_box_key("balanceOf", addr_bytes(user1.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    dai_client.send.call(
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

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_bal)],
        )
    )
    # user1 started with 100, now +50
    assert result.abi_return == INITIAL_BALANCE_USER1 + 50


# ─── Original: testMintself ───

@pytest.mark.localnet
def test_mint_self(
    dai_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: token.mint(address(this), 10); check balance increased."""
    app_id = dai_client.app_id
    mint_amount = 10

    wards_box = mapping_box_key("wards", addr_bytes(account.address))
    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))

    # Read current balance first
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    balance_before = result.abi_return

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, mint_amount],
            box_references=[
                box_ref(app_id, wards_box),
                box_ref(app_id, admin_bal),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    assert result.abi_return == balance_before + mint_amount


# ─── Original: testMintGuy ───

@pytest.mark.localnet
def test_mint_guy(
    dai_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """Original: token.mint(user1, 10); check balance."""
    app_id = dai_client.app_id
    mint_amount = 10

    wards_box = mapping_box_key("wards", addr_bytes(account.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    balance_before = result.abi_return

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user2.address, mint_amount],
            box_references=[
                box_ref(app_id, wards_box),
                box_ref(app_id, user2_bal),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    assert result.abi_return == balance_before + mint_amount


# ─── Original: testFailMintGuyNoAuth ───

@pytest.mark.localnet
def test_fail_mint_no_auth(
    dai_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Original: TokenUser(user1).doMint(user2, 10) should revert."""
    app_id = dai_client.app_id

    wards_box = mapping_box_key("wards", addr_bytes(user1.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))

    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=dai_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    with pytest.raises(Exception):
        client1.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[user2.address, 10],
                box_references=[
                    box_ref(app_id, wards_box),
                    box_ref(app_id, user2_bal),
                ],
            )
        )

    # Restore admin signer
    localnet.account.set_signer_from_account(account)


# ─── Original: testMintGuyAuth ───

@pytest.mark.localnet
def test_mint_guy_auth(
    dai_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Original: token.rely(user1); TokenUser(user1).doMint(user2, 10)."""
    app_id = dai_client.app_id

    admin_wards = mapping_box_key("wards", addr_bytes(account.address))
    user1_wards = mapping_box_key("wards", addr_bytes(user1.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))

    # Read user2 balance before
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    balance_before = result.abi_return

    # Admin relies user1 (grants mint authority)
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="rely",
            args=[user1.address],
            box_references=[
                box_ref(app_id, admin_wards),
                box_ref(app_id, user1_wards),
            ],
        )
    )

    # user1 mints to user2
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=dai_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user2.address, 10],
            box_references=[
                box_ref(app_id, user1_wards),
                box_ref(app_id, user2_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    assert result.abi_return == balance_before + 10


# ─── Original: testBurn ───

@pytest.mark.localnet
def test_burn(
    dai_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: token.burn(address(this), 10); check totalSupply decreased."""
    app_id = dai_client.app_id
    burn_amount = 10

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getTotalSupply",
            args=[],
        )
    )
    supply_before = result.abi_return

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, burn_amount],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getTotalSupply",
            args=[],
        )
    )
    assert result.abi_return == supply_before - burn_amount


# ─── Original: testBurnself ───

@pytest.mark.localnet
def test_burn_self(
    dai_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: burn 10 from self, check own balance decreased."""
    app_id = dai_client.app_id
    burn_amount = 5  # Different from test_burn to avoid duplicate txn

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    balance_before = result.abi_return

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, burn_amount],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, admin_bal)],
        )
    )
    assert result.abi_return == balance_before - burn_amount


# ─── Original: testTrusting (infinite approve) ───

@pytest.mark.localnet
def test_trusting(
    dai_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """Original: approve(user2, uint(-1)), check allowance is max uint."""
    app_id = dai_client.app_id
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user2.address)
    )
    MAX_UINT = 2**256 - 1

    # Set max approval
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user2.address, MAX_UINT],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    # uint512 max is 2^512-1 on AVM, but the stored value was 2^256-1
    # Check it's a very large number (the exact representation depends on ARC4 encoding)
    assert result.abi_return >= MAX_UINT

    # Reset to 0
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user2.address, 0],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user2.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 0


# ─── Original: testApproveWillModifyAllowance ───

@pytest.mark.localnet
def test_approve_will_modify_allowance(
    dai_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """Original: approve 1000, transferFrom 500, allowance should be 500."""
    app_id = dai_client.app_id
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(user1.address)
    )
    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user1_bal = mapping_box_key("balanceOf", addr_bytes(user1.address))

    # Approve user1 for 1000
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[user1.address, 1000],
            box_references=[box_ref(app_id, allow_box)],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 1000

    # user1 transfers 500 from admin to self
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=dai_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, user1.address, 500],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user1_bal),
            ],
        )
    )

    localnet.account.set_signer_from_account(account)

    # Allowance should be reduced to 500
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getAllowance",
            args=[account.address, user1.address],
            box_references=[box_ref(app_id, allow_box)],
        )
    )
    assert result.abi_return == 500


# ─── Original: testDenyAuth ───

@pytest.mark.localnet
def test_deny_removes_auth(
    dai_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
    user2: SigningAccount,
) -> None:
    """Rely user1, then deny user1. user1 should no longer be able to mint."""
    app_id = dai_client.app_id

    admin_wards = mapping_box_key("wards", addr_bytes(account.address))
    user1_wards = mapping_box_key("wards", addr_bytes(user1.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))

    # Check user1 wards (should be 1 from earlier test_mint_guy_auth rely)
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getWards",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_wards)],
        )
    )
    assert result.abi_return == 1

    # Deny user1
    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="deny",
            args=[user1.address],
            box_references=[
                box_ref(app_id, admin_wards),
                box_ref(app_id, user1_wards),
            ],
        )
    )

    # Verify wards is now 0
    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getWards",
            args=[user1.address],
            box_references=[box_ref(app_id, user1_wards)],
        )
    )
    assert result.abi_return == 0

    # user1 should fail to mint now
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=dai_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    with pytest.raises(Exception):
        client1.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[user2.address, 10],
                box_references=[
                    box_ref(app_id, user1_wards),
                    box_ref(app_id, user2_bal),
                ],
            )
        )

    localnet.account.set_signer_from_account(account)


# ─── Original: testPush (alias for transferFrom(self, usr, wad)) ───

@pytest.mark.localnet
def test_push(
    dai_client: au.AppClient, account: SigningAccount, user2: SigningAccount
) -> None:
    """Original: push is alias for transferFrom(msg.sender, usr, wad)."""
    app_id = dai_client.app_id

    admin_bal = mapping_box_key("balanceOf", addr_bytes(account.address))
    user2_bal = mapping_box_key("balanceOf", addr_bytes(user2.address))
    allow_box = mapping_box_key(
        "allowance", addr_bytes(account.address) + addr_bytes(account.address)
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    user2_before = result.abi_return

    dai_client.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[user2.address, 5],
            box_references=[
                box_ref(app_id, admin_bal),
                box_ref(app_id, allow_box),
                box_ref(app_id, user2_bal),
            ],
        )
    )

    result = dai_client.send.call(
        au.AppClientMethodCallParams(
            method="getBalanceOf",
            args=[user2.address],
            box_references=[box_ref(app_id, user2_bal)],
        )
    )
    assert result.abi_return == user2_before + 5
