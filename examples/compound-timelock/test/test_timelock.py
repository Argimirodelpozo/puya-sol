"""
Compound Finance Timelock — Test Suite
Translated from: https://github.com/compound-finance/compound-protocol/blob/master/tests/TimelockTest.js

Original tests by Compound Labs (BSD-3-Clause license).
Translated to Python/pytest for Algorand VM testing via puya-sol.

Tests cover: constructor, setDelay, setPendingAdmin, acceptAdmin,
queueTransaction, cancelTransaction, admin access control.

Skipped tests (require unsupported features):
- executeTransaction tests: Original uses `.call{value:}(callData)` for
  cross-contract execution. Removed from AVM-adapted contract.
- "requires msg.sender to be Timelock": Original has setDelay/setPendingAdmin
  require `msg.sender == address(this)`. Adapted to require admin for AVM
  (self-calls not practical on AVM).
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
def pending_admin(localnet: au.AlgorandClient) -> SigningAccount:
    """Account to be used as pending admin."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def timelock_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy Timelock with delay=50 (matching original's 2 days equivalent)."""
    client = deploy_contract(
        localnet, account, "Timelock",
        app_args=[pad64(50)],  # delay = 50 blocks
        fund_amount=1_000_000,
    )
    return client


# ─── Original: constructor tests ───

@pytest.mark.localnet
def test_sets_address_of_admin(
    timelock_client: au.AppClient, account: SigningAccount
) -> None:
    """Original: it('sets address of admin')"""
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getAdmin",
            args=[],
        )
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_sets_delay(timelock_client: au.AppClient) -> None:
    """Original: it('sets delay')"""
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getDelay",
            args=[],
        )
    )
    assert result.abi_return == 50


# ─── Original: setDelay tests ───

@pytest.mark.localnet
def test_set_delay(timelock_client: au.AppClient) -> None:
    """Admin can update delay within bounds."""
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="setDelay",
            args=[75],
        )
    )

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getDelay",
            args=[],
        )
    )
    assert result.abi_return == 75

    # Reset to 50
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="setDelay",
            args=[50],
        )
    )


@pytest.mark.localnet
def test_set_delay_requires_admin(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    pending_admin: SigningAccount,
) -> None:
    """Original: it('requires msg.sender to be Timelock')"""
    app_id = timelock_client.app_id
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=timelock_client.app_spec,
            app_id=app_id,
            default_sender=pending_admin.address,
        )
    )
    localnet.account.set_signer_from_account(pending_admin)

    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="setDelay",
                args=[60],
            )
        )

    localnet.account.set_signer_from_account(account)


# ─── Original: setPendingAdmin tests ───

@pytest.mark.localnet
def test_set_pending_admin(
    timelock_client: au.AppClient, pending_admin: SigningAccount
) -> None:
    """Original: full setPendingAdmin flow."""
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="setPendingAdmin",
            args=[pending_admin.address],
        )
    )

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getPendingAdmin",
            args=[],
        )
    )
    assert result.abi_return == pending_admin.address


@pytest.mark.localnet
def test_set_pending_admin_requires_admin(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    pending_admin: SigningAccount,
) -> None:
    """Non-admin cannot set pending admin."""
    app_id = timelock_client.app_id
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=timelock_client.app_spec,
            app_id=app_id,
            default_sender=pending_admin.address,
        )
    )
    localnet.account.set_signer_from_account(pending_admin)

    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="setPendingAdmin",
                args=[pending_admin.address],
            )
        )

    localnet.account.set_signer_from_account(account)


# ─── Original: acceptAdmin tests ───

@pytest.mark.localnet
def test_accept_admin_requires_pending(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> None:
    """Original: it('requires msg.sender to be pendingAdmin')"""
    # admin (not pendingAdmin) tries to accept → should fail
    with pytest.raises(Exception):
        timelock_client.send.call(
            au.AppClientMethodCallParams(
                method="acceptAdmin",
                args=[],
            )
        )


@pytest.mark.localnet
def test_accept_admin(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    pending_admin: SigningAccount,
) -> None:
    """Original: it('sets pendingAdmin to address 0 and changes admin')"""
    app_id = timelock_client.app_id

    # pending_admin accepts
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=timelock_client.app_spec,
            app_id=app_id,
            default_sender=pending_admin.address,
        )
    )
    localnet.account.set_signer_from_account(pending_admin)

    client2.send.call(
        au.AppClientMethodCallParams(
            method="acceptAdmin",
            args=[],
        )
    )

    # Verify admin changed
    result = client2.send.call(
        au.AppClientMethodCallParams(
            method="getAdmin",
            args=[],
        )
    )
    assert result.abi_return == pending_admin.address

    # Verify pendingAdmin cleared (address(0))
    result = client2.send.call(
        au.AppClientMethodCallParams(
            method="getPendingAdmin",
            args=[],
        )
    )
    # address(0) on Algorand = zero address
    assert result.abi_return == encoding.encode_address(b'\x00' * 32)

    # Transfer admin back to original account for remaining tests
    client2.send.call(
        au.AppClientMethodCallParams(
            method="setPendingAdmin",
            args=[account.address],
        )
    )

    localnet.account.set_signer_from_account(account)

    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="acceptAdmin",
            args=[],
        )
    )


# ─── Original: queueTransaction tests ───

@pytest.mark.localnet
def test_queue_transaction(
    timelock_client: au.AppClient, account: SigningAccount, pending_admin: SigningAccount
) -> None:
    """Original: it('sets hash as true in queuedTransactions mapping')"""
    app_id = timelock_client.app_id

    queued_box = mapping_box_key("_queuedTransactions", pad64(1))
    target_box = mapping_box_key("_txTarget", pad64(1))
    value_box = mapping_box_key("_txValue", pad64(1))
    eta_box = mapping_box_key("_txEta", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="queueTransaction",
            args=[pending_admin.address, 0, 200],  # target, value, eta
            box_references=[
                box_ref(app_id, queued_box),
                box_ref(app_id, target_box),
                box_ref(app_id, value_box),
                box_ref(app_id, eta_box),
            ],
        )
    )
    assert result.abi_return == 1

    # Verify queued
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isQueued",
            args=[1],
            box_references=[box_ref(app_id, queued_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_queue_transaction_requires_admin(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    pending_admin: SigningAccount,
) -> None:
    """Original: it('requires admin to be msg.sender')"""
    app_id = timelock_client.app_id

    queued_box = mapping_box_key("_queuedTransactions", pad64(2))
    target_box = mapping_box_key("_txTarget", pad64(2))
    value_box = mapping_box_key("_txValue", pad64(2))
    eta_box = mapping_box_key("_txEta", pad64(2))

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=timelock_client.app_spec,
            app_id=app_id,
            default_sender=pending_admin.address,
        )
    )
    localnet.account.set_signer_from_account(pending_admin)

    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="queueTransaction",
                args=[pending_admin.address, 0, 300],
                box_references=[
                    box_ref(app_id, queued_box),
                    box_ref(app_id, target_box),
                    box_ref(app_id, value_box),
                    box_ref(app_id, eta_box),
                ],
            )
        )

    localnet.account.set_signer_from_account(account)


@pytest.mark.localnet
def test_queue_transaction_data(
    timelock_client: au.AppClient, pending_admin: SigningAccount
) -> None:
    """Verify stored transaction data matches."""
    app_id = timelock_client.app_id

    target_box = mapping_box_key("_txTarget", pad64(1))
    value_box = mapping_box_key("_txValue", pad64(1))
    eta_box = mapping_box_key("_txEta", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getTarget",
            args=[1],
            box_references=[box_ref(app_id, target_box)],
        )
    )
    assert result.abi_return == pending_admin.address

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getValue",
            args=[1],
            box_references=[box_ref(app_id, value_box)],
        )
    )
    assert result.abi_return == 0

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getEta",
            args=[1],
            box_references=[box_ref(app_id, eta_box)],
        )
    )
    assert result.abi_return == 200


# ─── Original: cancelTransaction tests ───

@pytest.mark.localnet
def test_cancel_transaction(
    timelock_client: au.AppClient, account: SigningAccount, pending_admin: SigningAccount
) -> None:
    """Original: it('sets hash from true to false in queuedTransactions mapping')"""
    app_id = timelock_client.app_id

    # Queue tx 2
    queued_box2 = mapping_box_key("_queuedTransactions", pad64(2))
    target_box2 = mapping_box_key("_txTarget", pad64(2))
    value_box2 = mapping_box_key("_txValue", pad64(2))
    eta_box2 = mapping_box_key("_txEta", pad64(2))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="queueTransaction",
            args=[pending_admin.address, 100, 300],
            box_references=[
                box_ref(app_id, queued_box2),
                box_ref(app_id, target_box2),
                box_ref(app_id, value_box2),
                box_ref(app_id, eta_box2),
            ],
        )
    )
    assert result.abi_return == 2

    # Cancel tx 2
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="cancelTransaction",
            args=[2],
            box_references=[box_ref(app_id, queued_box2)],
        )
    )

    # Verify not queued
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isQueued",
            args=[2],
            box_references=[box_ref(app_id, queued_box2)],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_cancel_requires_admin(
    timelock_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    pending_admin: SigningAccount,
) -> None:
    """Non-admin cannot cancel."""
    app_id = timelock_client.app_id
    queued_box = mapping_box_key("_queuedTransactions", pad64(1))

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=timelock_client.app_spec,
            app_id=app_id,
            default_sender=pending_admin.address,
        )
    )
    localnet.account.set_signer_from_account(pending_admin)

    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="cancelTransaction",
                args=[1],
                box_references=[box_ref(app_id, queued_box)],
            )
        )

    localnet.account.set_signer_from_account(account)


# ─── Original: queue and cancel empty ───

@pytest.mark.localnet
def test_tx_count(timelock_client: au.AppClient) -> None:
    """Verify transaction count after queue operations."""
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getTxCount",
            args=[],
        )
    )
    assert result.abi_return == 2  # 2 transactions queued total
