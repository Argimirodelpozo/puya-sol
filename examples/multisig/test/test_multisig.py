"""
MultiSigWallet tests.
Tests: signer management, transaction submission, confirmation, revocation,
execution, requirement changes.
Exercises: nested mappings, counter patterns, multi-step state transitions,
role-based access control, view functions.
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
def account2(localnet: au.AlgorandClient) -> SigningAccount:
    """Create and fund a second account."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def account3(localnet: au.AlgorandClient) -> SigningAccount:
    """Create and fund a third account."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def multisig_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    account2: SigningAccount,
    account3: SigningAccount,
) -> au.AppClient:
    """Deploy MultiSigWallet with required=2 and 3 signers."""
    client = deploy_contract(
        localnet, account, "MultiSigWallet",
        app_args=[pad64(2)],  # required = 2
        fund_amount=1_000_000,
    )
    app_id = client.app_id

    # Add 3 signers: account, account2, account3
    for acct in [account, account2, account3]:
        signer_box = mapping_box_key("_isSigner", addr_bytes(acct.address))
        client.send.call(
            au.AppClientMethodCallParams(
                method="addSigner",
                args=[acct.address],
                box_references=[box_ref(app_id, signer_box)],
            )
        )

    return client


# ─── Signer Tests ───

@pytest.mark.localnet
def test_is_signer(
    multisig_client: au.AppClient, account: SigningAccount
) -> None:
    """Owner should be a signer."""
    app_id = multisig_client.app_id
    signer_box = mapping_box_key("_isSigner", addr_bytes(account.address))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="isSigner",
            args=[account.address],
            box_references=[box_ref(app_id, signer_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_signer_count(multisig_client: au.AppClient) -> None:
    """Should have 3 signers."""
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getSignerCount",
            args=[],
        )
    )
    assert result.abi_return == 3


@pytest.mark.localnet
def test_required(multisig_client: au.AppClient) -> None:
    """Required should be 2."""
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getRequired",
            args=[],
        )
    )
    assert result.abi_return == 2


# ─── Submit Transaction Tests ───

@pytest.mark.localnet
def test_submit_transaction(
    multisig_client: au.AppClient, account: SigningAccount, account2: SigningAccount
) -> None:
    """Submit a transaction and verify txId=1."""
    app_id = multisig_client.app_id
    addr_b = addr_bytes(account.address)

    signer_box = mapping_box_key("_isSigner", addr_b)
    dest_box = mapping_box_key("_txDestination", pad64(1))
    value_box = mapping_box_key("_txValue", pad64(1))
    exec_box = mapping_box_key("_txExecuted", pad64(1))
    conf_box = mapping_box_key("_txConfirmationCount", pad64(1))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="submitTransaction",
            args=[account2.address, 1000],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, dest_box),
                box_ref(app_id, value_box),
                box_ref(app_id, exec_box),
                box_ref(app_id, conf_box),
            ],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_transaction_count(multisig_client: au.AppClient) -> None:
    """Transaction count should be 1."""
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getTransactionCount",
            args=[],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_get_destination(
    multisig_client: au.AppClient, account2: SigningAccount
) -> None:
    """Destination should be account2."""
    app_id = multisig_client.app_id
    dest_box = mapping_box_key("_txDestination", pad64(1))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getDestination",
            args=[1],
            box_references=[box_ref(app_id, dest_box)],
        )
    )
    assert result.abi_return == account2.address


@pytest.mark.localnet
def test_get_value(multisig_client: au.AppClient) -> None:
    """Value should be 1000."""
    app_id = multisig_client.app_id
    value_box = mapping_box_key("_txValue", pad64(1))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[1],
            box_references=[box_ref(app_id, value_box)],
        )
    )
    assert result.abi_return == 1000


# ─── Confirmation Tests ───

@pytest.mark.localnet
def test_confirm_transaction(
    multisig_client: au.AppClient, account: SigningAccount
) -> None:
    """First signer confirms tx 1."""
    app_id = multisig_client.app_id
    addr_b = addr_bytes(account.address)

    signer_box = mapping_box_key("_isSigner", addr_b)
    exec_box = mapping_box_key("_txExecuted", pad64(1))
    confirm_box = mapping_box_key("_confirmations", pad64(1) + addr_b)
    count_box = mapping_box_key("_txConfirmationCount", pad64(1))

    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="confirmTransaction",
            args=[1],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, exec_box),
                box_ref(app_id, confirm_box),
                box_ref(app_id, count_box),
            ],
        )
    )


@pytest.mark.localnet
def test_is_confirmed_true(
    multisig_client: au.AppClient, account: SigningAccount
) -> None:
    """Account should have confirmed tx 1."""
    app_id = multisig_client.app_id
    confirm_box = mapping_box_key(
        "_confirmations", pad64(1) + addr_bytes(account.address)
    )

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="isConfirmed",
            args=[1, account.address],
            box_references=[box_ref(app_id, confirm_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_confirmation_count(multisig_client: au.AppClient) -> None:
    """Confirmation count should be 1."""
    app_id = multisig_client.app_id
    count_box = mapping_box_key("_txConfirmationCount", pad64(1))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getConfirmationCount",
            args=[1],
            box_references=[box_ref(app_id, count_box)],
        )
    )
    assert result.abi_return == 1


# ─── Second Confirmation + Execute ───

@pytest.mark.localnet
def test_second_confirm_and_execute(
    multisig_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    account2: SigningAccount,
) -> None:
    """Second signer confirms, then execute the transaction."""
    app_id = multisig_client.app_id
    addr_b2 = addr_bytes(account2.address)

    signer_box2 = mapping_box_key("_isSigner", addr_b2)
    exec_box = mapping_box_key("_txExecuted", pad64(1))
    confirm_box2 = mapping_box_key("_confirmations", pad64(1) + addr_b2)
    count_box = mapping_box_key("_txConfirmationCount", pad64(1))

    # Second confirmation from account2
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=multisig_client.app_spec,
            app_id=app_id,
            default_sender=account2.address,
        )
    )
    localnet.account.set_signer_from_account(account2)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="confirmTransaction",
            args=[1],
            box_references=[
                box_ref(app_id, signer_box2),
                box_ref(app_id, exec_box),
                box_ref(app_id, confirm_box2),
                box_ref(app_id, count_box),
            ],
        )
    )

    # Switch back to account1 for execution
    localnet.account.set_signer_from_account(account)

    # Execute
    addr_b = addr_bytes(account.address)
    signer_box = mapping_box_key("_isSigner", addr_b)

    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="executeTransaction",
            args=[1],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, exec_box),
                box_ref(app_id, count_box),
            ],
        )
    )


@pytest.mark.localnet
def test_is_executed(multisig_client: au.AppClient) -> None:
    """Transaction should be executed."""
    app_id = multisig_client.app_id
    exec_box = mapping_box_key("_txExecuted", pad64(1))

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="isExecuted",
            args=[1],
            box_references=[box_ref(app_id, exec_box)],
        )
    )
    assert result.abi_return is True


# ─── Revoke Tests ───

@pytest.mark.localnet
def test_submit_and_revoke(
    multisig_client: au.AppClient, account: SigningAccount
) -> None:
    """Submit tx 2, confirm, then revoke confirmation."""
    app_id = multisig_client.app_id
    addr_b = addr_bytes(account.address)

    signer_box = mapping_box_key("_isSigner", addr_b)
    dest_box2 = mapping_box_key("_txDestination", pad64(2))
    value_box2 = mapping_box_key("_txValue", pad64(2))
    exec_box2 = mapping_box_key("_txExecuted", pad64(2))
    conf_count_box2 = mapping_box_key("_txConfirmationCount", pad64(2))
    confirm_box = mapping_box_key("_confirmations", pad64(2) + addr_b)

    # Submit
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="submitTransaction",
            args=[account.address, 500],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, dest_box2),
                box_ref(app_id, value_box2),
                box_ref(app_id, exec_box2),
                box_ref(app_id, conf_count_box2),
            ],
        )
    )
    assert result.abi_return == 2

    # Confirm
    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="confirmTransaction",
            args=[2],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, exec_box2),
                box_ref(app_id, confirm_box),
                box_ref(app_id, conf_count_box2),
            ],
        )
    )

    # Verify confirmed
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getConfirmationCount",
            args=[2],
            box_references=[box_ref(app_id, conf_count_box2)],
        )
    )
    assert result.abi_return == 1

    # Revoke
    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="revokeConfirmation",
            args=[2],
            box_references=[
                box_ref(app_id, signer_box),
                box_ref(app_id, exec_box2),
                box_ref(app_id, confirm_box),
                box_ref(app_id, conf_count_box2),
            ],
        )
    )

    # Verify revoked
    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getConfirmationCount",
            args=[2],
            box_references=[box_ref(app_id, conf_count_box2)],
        )
    )
    assert result.abi_return == 0

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="isConfirmed",
            args=[2, account.address],
            box_references=[box_ref(app_id, confirm_box)],
        )
    )
    assert result.abi_return is False


# ─── Admin Tests ───

@pytest.mark.localnet
def test_change_requirement(multisig_client: au.AppClient) -> None:
    """Owner can change the requirement."""
    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="changeRequirement",
            args=[3],
        )
    )

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getRequired",
            args=[],
        )
    )
    assert result.abi_return == 3

    # Change back to 2
    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="changeRequirement",
            args=[2],
        )
    )


@pytest.mark.localnet
def test_remove_signer(
    multisig_client: au.AppClient, account3: SigningAccount
) -> None:
    """Owner can remove a signer."""
    app_id = multisig_client.app_id
    signer_box3 = mapping_box_key("_isSigner", addr_bytes(account3.address))

    multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="removeSigner",
            args=[account3.address],
            box_references=[box_ref(app_id, signer_box3)],
        )
    )

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="getSignerCount",
            args=[],
        )
    )
    assert result.abi_return == 2

    result = multisig_client.send.call(
        au.AppClientMethodCallParams(
            method="isSigner",
            args=[account3.address],
            box_references=[box_ref(app_id, signer_box3)],
        )
    )
    assert result.abi_return is False
