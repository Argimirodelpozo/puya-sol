"""
RateLimiter behavioral tests.
Tests rate limiting per address with configurable windows and max actions.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def count_key(addr):
    return mapping_box_key("_actionCount", encoding.decode_address(addr))


def window_key(addr):
    return mapping_box_key("_windowStart", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def limiter(localnet, account):
    return deploy_contract(localnet, account, "RateLimiterTest")


def test_deploy(limiter):
    assert limiter.app_id > 0


def test_owner(limiter, account):
    result = limiter.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_max_actions(limiter):
    result = limiter.send.call(
        au.AppClientMethodCallParams(method="maxActions")
    )
    assert result.abi_return == 5


def test_initial_window_size(limiter):
    result = limiter.send.call(
        au.AppClientMethodCallParams(method="windowSize")
    )
    assert result.abi_return == 100


def test_can_perform_initial(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="canPerformAction",
            args=[account.address, 1000],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_perform_first_action(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="performAction",
            args=[account.address, 1000],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_action_count_after(limiter, account):
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="actionCount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=count_key(account.address))],
        )
    )
    assert result.abi_return == 1


def test_remaining_actions(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="remainingActions",
            args=[account.address, 1010],
            box_references=boxes,
        )
    )
    assert result.abi_return == 4


def test_perform_more_actions(limiter, account):
    """Perform 4 more actions to reach limit."""
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    for i in range(4):
        limiter.send.call(
            au.AppClientMethodCallParams(
                method="performAction",
                args=[account.address, 1020 + i],
                box_references=boxes,
                note=f"action_{i+2}".encode(),
            )
        )


def test_remaining_after_max(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="remainingActions",
            args=[account.address, 1050],
            box_references=boxes,
            note=b"remaining2",
        )
    )
    assert result.abi_return == 0


def test_cannot_perform_at_limit(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="canPerformAction",
            args=[account.address, 1050],
            box_references=boxes,
            note=b"can2",
        )
    )
    assert result.abi_return is False


def test_can_perform_after_window(limiter, account):
    """After window expires, should be able to act again."""
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="canPerformAction",
            args=[account.address, 1200],  # past window
            box_references=boxes,
            note=b"can3",
        )
    )
    assert result.abi_return is True


def test_perform_in_new_window(limiter, account):
    boxes = [
        au.BoxReference(app_id=0, name=window_key(account.address)),
        au.BoxReference(app_id=0, name=count_key(account.address)),
    ]
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="performAction",
            args=[account.address, 1200],
            box_references=boxes,
            note=b"new_window",
        )
    )
    assert result.abi_return is True


def test_count_reset_in_new_window(limiter, account):
    result = limiter.send.call(
        au.AppClientMethodCallParams(
            method="actionCount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=count_key(account.address))],
            note=b"count2",
        )
    )
    assert result.abi_return == 1


def test_set_max_actions(limiter):
    limiter.send.call(
        au.AppClientMethodCallParams(
            method="setMaxActions",
            args=[10],
        )
    )
    result = limiter.send.call(
        au.AppClientMethodCallParams(method="maxActions", note=b"ma2")
    )
    assert result.abi_return == 10
