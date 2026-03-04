"""
TokenVesting tests.
Tests: schedule creation, vested amount calculation (before cliff, during linear,
after full), release, revocation, multi-schedule management.
Exercises: flat mappings, arithmetic with block numbers, linear interpolation.
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
def beneficiary(localnet: au.AlgorandClient) -> SigningAccount:
    """Create and fund a beneficiary account."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def vesting_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    beneficiary: SigningAccount,
) -> au.AppClient:
    """Deploy TokenVesting and create a schedule:
    - beneficiary: beneficiary account
    - totalAmount: 1000
    - cliffDuration: 50 blocks
    - vestingDuration: 200 blocks
    - startBlock: 100 (hardcoded in contract)
    So cliff ends at block 150, full vesting at block 300.
    """
    client = deploy_contract(
        localnet, account, "TokenVesting",
        fund_amount=1_000_000,
    )
    app_id = client.app_id

    # Create schedule
    id_key = pad64(1)
    beneficiary_box = mapping_box_key("_scheduleBeneficiary", id_key)
    total_box = mapping_box_key("_scheduleTotalAmount", id_key)
    start_box = mapping_box_key("_scheduleStartBlock", id_key)
    cliff_box = mapping_box_key("_scheduleCliffDuration", id_key)
    duration_box = mapping_box_key("_scheduleVestingDuration", id_key)
    released_box = mapping_box_key("_scheduleReleasedAmount", id_key)
    revoked_box = mapping_box_key("_scheduleRevoked", id_key)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="createSchedule",
            args=[beneficiary.address, 1000, 50, 200],
            box_references=[
                box_ref(app_id, beneficiary_box),
                box_ref(app_id, total_box),
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, duration_box),
                box_ref(app_id, released_box),
                box_ref(app_id, revoked_box),
            ],
        )
    )
    assert result.abi_return == 1

    return client


# ─── Schedule Creation Tests ───

@pytest.mark.localnet
def test_schedule_count(vesting_client: au.AppClient) -> None:
    """Should have 1 schedule."""
    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getScheduleCount",
            args=[],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_beneficiary(
    vesting_client: au.AppClient, beneficiary: SigningAccount
) -> None:
    """Beneficiary should match."""
    app_id = vesting_client.app_id
    b_box = mapping_box_key("_scheduleBeneficiary", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getBeneficiary",
            args=[1],
            box_references=[box_ref(app_id, b_box)],
        )
    )
    assert result.abi_return == beneficiary.address


@pytest.mark.localnet
def test_total_amount(vesting_client: au.AppClient) -> None:
    """Total amount should be 1000."""
    app_id = vesting_client.app_id
    box = mapping_box_key("_scheduleTotalAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getTotalAmount",
            args=[1],
            box_references=[box_ref(app_id, box)],
        )
    )
    assert result.abi_return == 1000


@pytest.mark.localnet
def test_cliff_and_duration(vesting_client: au.AppClient) -> None:
    """Cliff=50, Duration=200."""
    app_id = vesting_client.app_id
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getCliffDuration",
            args=[1],
            box_references=[box_ref(app_id, cliff_box)],
        )
    )
    assert result.abi_return == 50

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getVestingDuration",
            args=[1],
            box_references=[box_ref(app_id, dur_box)],
        )
    )
    assert result.abi_return == 200


# ─── Vested Amount Calculation Tests ───

@pytest.mark.localnet
def test_vested_before_cliff(vesting_client: au.AppClient) -> None:
    """Before cliff (block 120 < 100+50=150): vested = 0."""
    app_id = vesting_client.app_id
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[1, 120],  # block 120, before cliff at 150
            box_references=[
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
            ],
        )
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_vested_at_cliff(vesting_client: au.AppClient) -> None:
    """At cliff (block 150 = 100+50): vested = 1000 * 50 / 200 = 250."""
    app_id = vesting_client.app_id
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[1, 150],  # block 150, exactly at cliff
            box_references=[
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
            ],
        )
    )
    assert result.abi_return == 250  # 1000 * 50/200


@pytest.mark.localnet
def test_vested_midway(vesting_client: au.AppClient) -> None:
    """At block 200 (halfway): vested = 1000 * 100 / 200 = 500."""
    app_id = vesting_client.app_id
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[1, 200],  # block 200, 100 blocks elapsed
            box_references=[
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
            ],
        )
    )
    assert result.abi_return == 500  # 1000 * 100/200


@pytest.mark.localnet
def test_vested_after_full(vesting_client: au.AppClient) -> None:
    """After full vesting (block 300+): vested = 1000."""
    app_id = vesting_client.app_id
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[1, 400],  # well past end
            box_references=[
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
            ],
        )
    )
    assert result.abi_return == 1000


# ─── Releasable Amount Tests ───

@pytest.mark.localnet
def test_releasable_at_cliff(vesting_client: au.AppClient) -> None:
    """Releasable at cliff = 250 (nothing released yet)."""
    app_id = vesting_client.app_id
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))
    released_box = mapping_box_key("_scheduleReleasedAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="releasableAmount",
            args=[1, 150],
            box_references=[
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
                box_ref(app_id, released_box),
            ],
        )
    )
    assert result.abi_return == 250


# ─── Release Tests ───

@pytest.mark.localnet
def test_release_at_midpoint(
    vesting_client: au.AppClient, account: SigningAccount
) -> None:
    """Release at block 200: should release 500."""
    app_id = vesting_client.app_id
    revoked_box = mapping_box_key("_scheduleRevoked", pad64(1))
    beneficiary_box = mapping_box_key("_scheduleBeneficiary", pad64(1))
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))
    released_box = mapping_box_key("_scheduleReleasedAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[1, 200],  # release at block 200
            box_references=[
                box_ref(app_id, revoked_box),
                box_ref(app_id, beneficiary_box),
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
                box_ref(app_id, released_box),
            ],
        )
    )
    assert result.abi_return == 500


@pytest.mark.localnet
def test_released_amount_after_release(vesting_client: au.AppClient) -> None:
    """Released should now be 500."""
    app_id = vesting_client.app_id
    box = mapping_box_key("_scheduleReleasedAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="getReleasedAmount",
            args=[1],
            box_references=[box_ref(app_id, box)],
        )
    )
    assert result.abi_return == 500


@pytest.mark.localnet
def test_release_remaining(
    vesting_client: au.AppClient, account: SigningAccount
) -> None:
    """Release at block 400 (full): should release remaining 500."""
    app_id = vesting_client.app_id
    revoked_box = mapping_box_key("_scheduleRevoked", pad64(1))
    beneficiary_box = mapping_box_key("_scheduleBeneficiary", pad64(1))
    start_box = mapping_box_key("_scheduleStartBlock", pad64(1))
    cliff_box = mapping_box_key("_scheduleCliffDuration", pad64(1))
    dur_box = mapping_box_key("_scheduleVestingDuration", pad64(1))
    total_box = mapping_box_key("_scheduleTotalAmount", pad64(1))
    released_box = mapping_box_key("_scheduleReleasedAmount", pad64(1))

    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[1, 400],
            box_references=[
                box_ref(app_id, revoked_box),
                box_ref(app_id, beneficiary_box),
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, total_box),
                box_ref(app_id, released_box),
            ],
        )
    )
    assert result.abi_return == 500  # 1000 - 500 already released


# ─── Revoke Tests ───

@pytest.mark.localnet
def test_revoke_schedule(
    vesting_client: au.AppClient, account: SigningAccount, beneficiary: SigningAccount
) -> None:
    """Create schedule 2, then revoke it."""
    app_id = vesting_client.app_id
    id_key = pad64(2)

    beneficiary_box = mapping_box_key("_scheduleBeneficiary", id_key)
    total_box = mapping_box_key("_scheduleTotalAmount", id_key)
    start_box = mapping_box_key("_scheduleStartBlock", id_key)
    cliff_box = mapping_box_key("_scheduleCliffDuration", id_key)
    dur_box = mapping_box_key("_scheduleVestingDuration", id_key)
    released_box = mapping_box_key("_scheduleReleasedAmount", id_key)
    revoked_box = mapping_box_key("_scheduleRevoked", id_key)

    # Create schedule 2
    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="createSchedule",
            args=[beneficiary.address, 500, 10, 100],
            box_references=[
                box_ref(app_id, beneficiary_box),
                box_ref(app_id, total_box),
                box_ref(app_id, start_box),
                box_ref(app_id, cliff_box),
                box_ref(app_id, dur_box),
                box_ref(app_id, released_box),
                box_ref(app_id, revoked_box),
            ],
        )
    )
    assert result.abi_return == 2

    # Revoke
    vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="revoke",
            args=[2],
            box_references=[box_ref(app_id, revoked_box)],
        )
    )

    # Verify revoked
    result = vesting_client.send.call(
        au.AppClientMethodCallParams(
            method="isRevoked",
            args=[2],
            box_references=[box_ref(app_id, revoked_box)],
        )
    )
    assert result.abi_return is True
