"""
TokenVesting behavioral tests.
Tests linear vesting schedule creation, release, and revocation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def total_key(addr):
    return mapping_box_key("_vestingTotal", encoding.decode_address(addr))


def released_key(addr):
    return mapping_box_key("_vestingReleased", encoding.decode_address(addr))


def start_key(addr):
    return mapping_box_key("_vestingStart", encoding.decode_address(addr))


def dur_key(addr):
    return mapping_box_key("_vestingDuration", encoding.decode_address(addr))


def all_vesting_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=total_key(addr)),
        au.BoxReference(app_id=0, name=released_key(addr)),
        au.BoxReference(app_id=0, name=start_key(addr)),
        au.BoxReference(app_id=0, name=dur_key(addr)),
    ]


@pytest.fixture(scope="module")
def vesting(localnet, account):
    return deploy_contract(localnet, account, "TokenVestingTest")


def test_deploy(vesting):
    assert vesting.app_id > 0


def test_owner(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_total_allocated(vesting):
    result = vesting.send.call(
        au.AppClientMethodCallParams(method="totalAllocated")
    )
    assert result.abi_return == 0


def test_create_vesting(vesting, account):
    boxes = all_vesting_boxes(account.address)
    vesting.send.call(
        au.AppClientMethodCallParams(
            method="createVesting",
            args=[account.address, 10000, 100, 1000],  # 10000 tokens, start=100, dur=1000
            box_references=boxes,
        )
    )


def test_vesting_total(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestingTotal",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=total_key(account.address))],
        )
    )
    assert result.abi_return == 10000


def test_total_allocated_after_create(vesting):
    result = vesting.send.call(
        au.AppClientMethodCallParams(method="totalAllocated", note=b"a1")
    )
    assert result.abi_return == 10000


def test_vested_before_start(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[account.address, 50],  # before start time 100
            box_references=[
                au.BoxReference(app_id=0, name=total_key(account.address)),
                au.BoxReference(app_id=0, name=start_key(account.address)),
                au.BoxReference(app_id=0, name=dur_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 0


def test_vested_at_half(vesting, account):
    # At time 600: (10000 * (600 - 100)) / 1000 = 5000
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[account.address, 600],
            box_references=[
                au.BoxReference(app_id=0, name=total_key(account.address)),
                au.BoxReference(app_id=0, name=start_key(account.address)),
                au.BoxReference(app_id=0, name=dur_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 5000


def test_vested_after_end(vesting, account):
    # At time 1200: past start + duration (1100), should be full amount
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestedAmount",
            args=[account.address, 1200],
            box_references=[
                au.BoxReference(app_id=0, name=total_key(account.address)),
                au.BoxReference(app_id=0, name=start_key(account.address)),
                au.BoxReference(app_id=0, name=dur_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 10000


def test_releasable_at_half(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="releasable",
            args=[account.address, 600],
            box_references=[
                au.BoxReference(app_id=0, name=total_key(account.address)),
                au.BoxReference(app_id=0, name=start_key(account.address)),
                au.BoxReference(app_id=0, name=dur_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
        )
    )
    assert result.abi_return == 5000


def test_release(vesting, account):
    boxes = [
        au.BoxReference(app_id=0, name=total_key(account.address)),
        au.BoxReference(app_id=0, name=start_key(account.address)),
        au.BoxReference(app_id=0, name=dur_key(account.address)),
        au.BoxReference(app_id=0, name=released_key(account.address)),
    ]
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="release",
            args=[account.address, 600],
            box_references=boxes,
        )
    )
    assert result.abi_return == 5000


def test_released_after(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestingReleased",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=released_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_releasable_after_partial_release(vesting, account):
    # At time 800: vested = (10000 * 700) / 1000 = 7000, released = 5000, releasable = 2000
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="releasable",
            args=[account.address, 800],
            box_references=[
                au.BoxReference(app_id=0, name=total_key(account.address)),
                au.BoxReference(app_id=0, name=start_key(account.address)),
                au.BoxReference(app_id=0, name=dur_key(account.address)),
                au.BoxReference(app_id=0, name=released_key(account.address)),
            ],
            note=b"releasable2",
        )
    )
    assert result.abi_return == 2000


def test_revoke_vesting(vesting, account):
    boxes = all_vesting_boxes(account.address)
    vesting.send.call(
        au.AppClientMethodCallParams(
            method="revokeVesting",
            args=[account.address],
            box_references=boxes,
        )
    )


def test_total_after_revoke(vesting, account):
    result = vesting.send.call(
        au.AppClientMethodCallParams(
            method="vestingTotal",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=total_key(account.address))],
            note=b"vt2",
        )
    )
    assert result.abi_return == 0


def test_allocated_after_revoke(vesting):
    # 10000 - (10000 - 5000) = 5000 already released, so totalAllocated -= 5000
    result = vesting.send.call(
        au.AppClientMethodCallParams(method="totalAllocated", note=b"a2")
    )
    assert result.abi_return == 5000
