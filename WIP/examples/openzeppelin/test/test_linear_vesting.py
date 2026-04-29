"""
LinearVestingTest behavioral tests.
Tests time-based linear vesting schedule computation.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def alloc_box(addr: str) -> bytes:
    return mapping_box_key("_totalAllocation", encoding.decode_address(addr))


def released_box(addr: str) -> bytes:
    return mapping_box_key("_released", encoding.decode_address(addr))


def start_box(addr: str) -> bytes:
    return mapping_box_key("_startTime", encoding.decode_address(addr))


def duration_box(addr: str) -> bytes:
    return mapping_box_key("_duration", encoding.decode_address(addr))


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def all_boxes(app_id: int, addr: str) -> list[au.BoxReference]:
    return [
        box_ref(app_id, alloc_box(addr)),
        box_ref(app_id, released_box(addr)),
        box_ref(app_id, start_box(addr)),
        box_ref(app_id, duration_box(addr)),
    ]


@pytest.fixture(scope="module")
def vesting(localnet, account):
    return deploy_contract(localnet, account, "LinearVestingTest")


@pytest.fixture(scope="module")
def beneficiary(account):
    return account.address


@pytest.fixture(scope="module")
def setup_vesting(vesting, beneficiary):
    """Create a vesting schedule: 10000 tokens, start=1000, duration=1000."""
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    vesting.send.call(
        au.AppClientMethodCallParams(
            method="createVesting",
            args=[beneficiary, 10000, 1000, 1000],
            box_references=boxes,
        )
    )
    return True


def test_deploy(vesting):
    assert vesting.app_id > 0


def test_allocation(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="allocation",
            args=[beneficiary],
            box_references=[box_ref(app_id, alloc_box(beneficiary))],
        )
    )
    assert result.abi_return == 10000


def test_duration(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="duration",
            args=[beneficiary],
            box_references=[box_ref(app_id, duration_box(beneficiary))],
        )
    )
    assert result.abi_return == 1000


def test_vested_before_start(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 500],  # before start (1000)
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_vested_at_start(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 1000],  # at start
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_vested_at_25_percent(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 1250],  # 25% through
            box_references=boxes,
        )
    )
    # 10000 * 250 / 1000 = 2500
    assert result.abi_return == 2500


def test_vested_at_50_percent(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 1500],  # 50% through
            box_references=boxes,
        )
    )
    assert result.abi_return == 5000


def test_vested_at_end(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 2000],  # at end
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000


def test_vested_after_end(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[beneficiary, 9999],  # well after end
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000


def test_releasable_at_half(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="releasableAmount",
            args=[beneficiary, 1500],
            box_references=boxes,
        )
    )
    assert result.abi_return == 5000


def test_release_at_half(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    vesting.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[beneficiary, 1500],
            box_references=boxes,
        )
    )
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="released",
            args=[beneficiary],
            box_references=[box_ref(app_id, released_box(beneficiary))],
        )
    )
    assert result.abi_return == 5000


def test_releasable_after_partial_release(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="releasableAmount",
            args=[beneficiary, 1750],  # 75% through
            box_references=boxes,
        )
    )
    # Vested: 10000 * 750/1000 = 7500, already released: 5000
    assert result.abi_return == 2500


def test_release_remaining(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    vesting.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[beneficiary, 3000],  # after vesting ends
            box_references=boxes,
        )
    )
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="released",
            args=[beneficiary],
            box_references=[box_ref(app_id, released_box(beneficiary))],
        )
    )
    assert result.abi_return == 10000


def test_nothing_to_release(vesting, beneficiary, setup_vesting):
    app_id = vesting.app_id
    boxes = all_boxes(app_id, beneficiary)
    with pytest.raises(Exception):
        vesting.send.call(
            au.AppClientMethodCallParams(
                method="release",
                args=[beneficiary, 5000],
                box_references=boxes,
            )
        )
