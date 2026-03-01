"""
DequeTest (stack) behavioral tests.
Tests mapping-based stack with uint256 keys.
This was the contract that exposed the biguint mapping key normalization bug.
"""

import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract


def data_box_key(idx_bytes: bytes) -> bytes:
    """Box key for _data[idx].
    idx_bytes must be exactly 64 bytes (biguint normalized).
    """
    key_hash = hashlib.sha256(idx_bytes).digest()
    return b"_data" + key_hash


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def idx_to_bytes(n: int) -> bytes:
    """Convert integer to 64-byte big-endian (matches biguint normalization)."""
    return n.to_bytes(64, "big")


@pytest.fixture(scope="module")
def deque(localnet, account):
    return deploy_contract(localnet, account, "DequeTest")


def test_deploy(deque):
    assert deque.app_id > 0


def test_initially_empty(deque):
    result = deque.send.call(
        au.AppClientMethodCallParams(method="empty", args=[])
    )
    assert result.abi_return is True


def test_initial_length_zero(deque):
    result = deque.send.call(
        au.AppClientMethodCallParams(method="length", args=[])
    )
    assert result.abi_return == 0


def test_push_one(deque):
    app_id = deque.app_id
    # push(42) stores at _data[0], _size becomes 1
    dk = data_box_key(idx_to_bytes(0))
    deque.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[42],
            box_references=[box_ref(app_id, dk)],
        )
    )
    result = deque.send.call(
        au.AppClientMethodCallParams(method="length", args=[])
    )
    assert result.abi_return == 1


def test_top_after_push(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(0))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="top",
            args=[],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 42


def test_at_zero(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(0))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="at",
            args=[0],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 42


def test_push_second(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(1))
    deque.send.call(
        au.AppClientMethodCallParams(
            method="push",
            args=[99],
            box_references=[box_ref(app_id, dk)],
        )
    )
    result = deque.send.call(
        au.AppClientMethodCallParams(method="length", args=[])
    )
    assert result.abi_return == 2


def test_top_after_second_push(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(1))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="top",
            args=[],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 99


def test_at_one(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(1))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="at",
            args=[1],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 99


def test_not_empty(deque):
    result = deque.send.call(
        au.AppClientMethodCallParams(method="empty", args=[])
    )
    assert result.abi_return is False


def test_pop_returns_last(deque):
    """Pop should return 99 (last pushed). This is the critical test
    for the biguint key normalization fix - pop computes _size-1 via b-
    which must produce the same 64-byte key as the itob used during push."""
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(1))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="pop",
            args=[],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 99


def test_length_after_pop(deque):
    result = deque.send.call(
        au.AppClientMethodCallParams(method="length", args=[])
    )
    assert result.abi_return == 1


def test_top_after_pop(deque):
    """After popping 99, top should be 42."""
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(0))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="top",
            args=[],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 42


def test_pop_last(deque):
    app_id = deque.app_id
    dk = data_box_key(idx_to_bytes(0))
    result = deque.send.call(
        au.AppClientMethodCallParams(
            method="pop",
            args=[],
            box_references=[box_ref(app_id, dk)],
        )
    )
    assert result.abi_return == 42


def test_empty_after_all_pops(deque):
    result = deque.send.call(
        au.AppClientMethodCallParams(method="empty", args=[])
    )
    assert result.abi_return is True


def test_pop_empty_fails(deque):
    with pytest.raises(Exception):
        deque.send.call(
            au.AppClientMethodCallParams(method="pop", args=[])
        )


def test_at_out_of_bounds(deque):
    with pytest.raises(Exception):
        deque.send.call(
            au.AppClientMethodCallParams(
                method="at",
                args=[0],
            )
        )
