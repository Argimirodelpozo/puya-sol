"""
OpenZeppelin Checkpoints behavioral tests (simplified mapping-based variant).
Tests push, latest, latestCheckpoint, getAtKey, and hasCheckpoint.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def values_key(key: int) -> bytes:
    """Box key for _values[key]. uint256 keys are 64-byte big-endian."""
    key_bytes = key.to_bytes(64, "big")
    return mapping_box_key("_values", key_bytes)


@pytest.fixture(scope="module")
def checkpoints(localnet, account):
    return deploy_contract(localnet, account, "CheckpointsTest")


def test_deploy(checkpoints):
    assert checkpoints.app_id > 0


def test_no_checkpoint_initially(checkpoints):
    """hasCheckpoint should return False before any push."""
    result = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="hasCheckpoint",
        )
    )
    assert result.abi_return is False


def test_push_single(checkpoints):
    """Push a single (key, value) checkpoint and verify return values."""
    key = 10
    value = 100
    vk = values_key(key)
    result = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[key, value],
            box_references=[box_ref(checkpoints.app_id, vk)],
        )
    )
    # Returns (oldValue, newValue). Old value is 0 since this is the first push.
    assert result.abi_return == [0, value]


def test_push_multiple(checkpoints):
    """Push additional checkpoints with non-decreasing keys."""
    vk20 = values_key(20)
    result1 = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[20, 200],
            box_references=[box_ref(checkpoints.app_id, vk20)],
        )
    )
    # Old value was 100 (from previous push of key=10, value=100)
    assert result1.abi_return == [100, 200]

    vk30 = values_key(30)
    result2 = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[30, 300],
            box_references=[box_ref(checkpoints.app_id, vk30)],
        )
    )
    assert result2.abi_return == [200, 300]


def test_latest(checkpoints):
    """latest() returns the most recently pushed value."""
    result = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="latest",
        )
    )
    # After pushing (10,100), (20,200), (30,300), latest value is 300
    assert result.abi_return == 300


def test_latest_checkpoint(checkpoints):
    """latestCheckpoint() returns (true, latestKey, latestValue)."""
    result = checkpoints.send.call(
        au.AppClientMethodCallParams(
            method="latestCheckpoint",
        )
    )
    # Returns (exists, key, value)
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["exists"] is True
        assert ret["key"] == 30
        assert ret["value"] == 300
    else:
        assert ret == [True, 30, 300]


def test_get_at_key(checkpoints):
    """getAtKey returns the stored value for each previously pushed key."""
    for key, expected_value in [(10, 100), (20, 200), (30, 300)]:
        vk = values_key(key)
        result = checkpoints.send.call(
            au.AppClientMethodCallParams(
                method="getAtKey",
                args=[key],
                box_references=[box_ref(checkpoints.app_id, vk)],
            )
        )
        assert result.abi_return == expected_value, (
            f"getAtKey({key}) expected {expected_value}, got {result.abi_return}"
        )
