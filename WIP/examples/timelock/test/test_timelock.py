"""
Timelock Controller tests.
Tests: scheduling operations with delay, executing, cancelling, role management.
Exercises: boolean mappings, role-based access control, operation lifecycle,
view functions, constant values, event emission.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref

UNSET = 0
PENDING = 1
DONE = 2


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def pad64(value: int) -> bytes:
    return value.to_bytes(64, "big")


@pytest.fixture(scope="module")
def timelock_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy TimelockController with min delay of 10 blocks."""
    min_delay = pad64(10)

    client = deploy_contract(
        localnet, account, "TimelockController",
        app_args=[min_delay],
        fund_amount=1_000_000,
    )

    # Grant proposer and executor roles to admin (not done in constructor)
    app_id = client.app_id
    addr_b = addr_bytes(account.address)
    proposer_box = mapping_box_key("_isProposer", addr_b)
    executor_box = mapping_box_key("_isExecutor", addr_b)

    client.send.call(
        au.AppClientMethodCallParams(
            method="grantProposerRole",
            args=[account.address],
            box_references=[box_ref(app_id, proposer_box)],
        )
    )

    client.send.call(
        au.AppClientMethodCallParams(
            method="grantExecutorRole",
            args=[account.address],
            box_references=[box_ref(app_id, executor_box)],
        )
    )

    return client


# ─── Role Tests ───

@pytest.mark.localnet
def test_is_proposer(timelock_client: au.AppClient, account: SigningAccount) -> None:
    """Admin should have proposer role."""
    app_id = timelock_client.app_id
    proposer_box = mapping_box_key("_isProposer", addr_bytes(account.address))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isProposer",
            args=[account.address],
            box_references=[box_ref(app_id, proposer_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_is_executor(timelock_client: au.AppClient, account: SigningAccount) -> None:
    """Admin should have executor role."""
    app_id = timelock_client.app_id
    executor_box = mapping_box_key("_isExecutor", addr_bytes(account.address))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isExecutor",
            args=[account.address],
            box_references=[box_ref(app_id, executor_box)],
        )
    )
    assert result.abi_return is True


# ─── Schedule Tests ───

@pytest.mark.localnet
def test_schedule_operation(
    timelock_client: au.AppClient, account: SigningAccount
) -> None:
    """Schedule an operation with delay=20 blocks."""
    app_id = timelock_client.app_id
    addr_b = addr_bytes(account.address)

    proposer_box = mapping_box_key("_isProposer", addr_b)
    state_box = mapping_box_key("_operationState", pad64(1))
    ready_box = mapping_box_key("_operationReadyAt", pad64(1))
    target_box = mapping_box_key("_operationTarget", pad64(1))
    value_box = mapping_box_key("_operationValue", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="schedule",
            args=[account.address, 0, 20],  # target=self, value=0, delay=20
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(app_id, state_box),
                box_ref(app_id, ready_box),
                box_ref(app_id, target_box),
                box_ref(app_id, value_box),
            ],
        )
    )
    assert result.abi_return == 1  # first operation


@pytest.mark.localnet
def test_operation_count(timelock_client: au.AppClient) -> None:
    """Operation count should be 1."""
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getOperationCount",
            args=[],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_operation_is_pending(timelock_client: au.AppClient) -> None:
    """Scheduled operation should be pending."""
    app_id = timelock_client.app_id
    state_box = mapping_box_key("_operationState", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isOperationPending",
            args=[1],
            box_references=[box_ref(app_id, state_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_operation_state(timelock_client: au.AppClient) -> None:
    """State should be PENDING (1)."""
    app_id = timelock_client.app_id
    state_box = mapping_box_key("_operationState", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getOperationState",
            args=[1],
            box_references=[box_ref(app_id, state_box)],
        )
    )
    assert result.abi_return == PENDING


@pytest.mark.localnet
def test_operation_ready_at(timelock_client: au.AppClient) -> None:
    """ReadyAt should be 100 + 20 = 120."""
    app_id = timelock_client.app_id
    ready_box = mapping_box_key("_operationReadyAt", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getOperationReadyAt",
            args=[1],
            box_references=[box_ref(app_id, ready_box)],
        )
    )
    assert result.abi_return == 120


# ─── Execute Tests ───

@pytest.mark.localnet
def test_execute_operation(
    timelock_client: au.AppClient, account: SigningAccount
) -> None:
    """Execute the pending operation."""
    app_id = timelock_client.app_id
    addr_b = addr_bytes(account.address)

    executor_box = mapping_box_key("_isExecutor", addr_b)
    state_box = mapping_box_key("_operationState", pad64(1))

    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="execute",
            args=[1],
            box_references=[
                box_ref(app_id, executor_box),
                box_ref(app_id, state_box),
            ],
        )
    )


@pytest.mark.localnet
def test_operation_is_done(timelock_client: au.AppClient) -> None:
    """After execution, operation should be DONE."""
    app_id = timelock_client.app_id
    state_box = mapping_box_key("_operationState", pad64(1))

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="isOperationDone",
            args=[1],
            box_references=[box_ref(app_id, state_box)],
        )
    )
    assert result.abi_return is True

    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getOperationState",
            args=[1],
            box_references=[box_ref(app_id, state_box)],
        )
    )
    assert result.abi_return == DONE


# ─── Cancel Tests ───

@pytest.mark.localnet
def test_schedule_and_cancel(
    timelock_client: au.AppClient, account: SigningAccount
) -> None:
    """Schedule operation 2 then cancel it."""
    app_id = timelock_client.app_id
    addr_b = addr_bytes(account.address)

    proposer_box = mapping_box_key("_isProposer", addr_b)
    state_box_2 = mapping_box_key("_operationState", pad64(2))
    ready_box_2 = mapping_box_key("_operationReadyAt", pad64(2))
    target_box_2 = mapping_box_key("_operationTarget", pad64(2))
    value_box_2 = mapping_box_key("_operationValue", pad64(2))

    # Schedule
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="schedule",
            args=[account.address, 0, 50],
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(app_id, state_box_2),
                box_ref(app_id, ready_box_2),
                box_ref(app_id, target_box_2),
                box_ref(app_id, value_box_2),
            ],
        )
    )
    assert result.abi_return == 2

    # Cancel
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="cancel",
            args=[2],
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(app_id, state_box_2),
            ],
        )
    )

    # Verify cancelled (state = UNSET)
    result = timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="getOperationState",
            args=[2],
            box_references=[box_ref(app_id, state_box_2)],
        )
    )
    assert result.abi_return == UNSET


# ─── Admin Tests ───

@pytest.mark.localnet
def test_update_min_delay(timelock_client: au.AppClient) -> None:
    """Admin can update the minimum delay."""
    timelock_client.send.call(
        au.AppClientMethodCallParams(
            method="updateMinDelay",
            args=[25],
        )
    )

    # Verify (no direct getter for minDelay, but we can schedule with old delay and it should fail)
    # Actually minDelay is a public state var — but puya-sol doesn't generate getters for those
    # So we just verify the function didn't crash
