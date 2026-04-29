"""
TokenBridge behavioral tests.
Tests initiation, completion, refund, pause, and fee management.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def sender_key(tid):
    return mapping_box_key("_transferSender", tid.to_bytes(64, "big"))


def amount_key(tid):
    return mapping_box_key("_transferAmount", tid.to_bytes(64, "big"))


def chain_key(tid):
    return mapping_box_key("_transferDestChain", tid.to_bytes(64, "big"))


def completed_key(tid):
    return mapping_box_key("_transferCompleted", tid.to_bytes(64, "big"))


def refunded_key(tid):
    return mapping_box_key("_transferRefunded", tid.to_bytes(64, "big"))


def transfer_boxes(tid):
    return [
        au.BoxReference(app_id=0, name=sender_key(tid)),
        au.BoxReference(app_id=0, name=amount_key(tid)),
        au.BoxReference(app_id=0, name=chain_key(tid)),
        au.BoxReference(app_id=0, name=completed_key(tid)),
        au.BoxReference(app_id=0, name=refunded_key(tid)),
    ]


@pytest.fixture(scope="module")
def bridge(localnet, account):
    return deploy_contract(localnet, account, "TokenBridgeTest")


def test_deploy(bridge):
    assert bridge.app_id > 0


def test_admin(bridge, account):
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_not_paused(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="isPaused")
    )
    assert result.abi_return is False


def test_initiate(bridge, account):
    # fee = 10000 * 100 / 10000 = 100, net = 9900
    boxes = transfer_boxes(1)
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="initiate",
            args=[account.address, 10000, 42],  # amount=10000, destChain=42
            box_references=boxes,
        )
    )
    assert result.abi_return == 1


def test_transfer_sender(bridge, account):
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="getTransferSender",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=sender_key(1))],
        )
    )
    assert result.abi_return == account.address


def test_transfer_amount(bridge):
    # net amount = 10000 - 100 = 9900
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="getTransferAmount",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=amount_key(1))],
        )
    )
    assert result.abi_return == 9900


def test_transfer_dest_chain(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="getTransferDestChain",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=chain_key(1))],
        )
    )
    assert result.abi_return == 42


def test_total_bridged(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="getTotalBridged")
    )
    assert result.abi_return == 9900


def test_total_fees(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="getTotalFees")
    )
    assert result.abi_return == 100


def test_complete_transfer(bridge):
    bridge.send.call(
        au.AppClientMethodCallParams(
            method="complete",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=completed_key(1)),
                au.BoxReference(app_id=0, name=refunded_key(1)),
            ],
        )
    )


def test_is_completed(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="isTransferCompleted",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=completed_key(1))],
        )
    )
    assert result.abi_return is True


def test_initiate_and_refund(bridge, account):
    boxes = transfer_boxes(2)
    bridge.send.call(
        au.AppClientMethodCallParams(
            method="initiate",
            args=[account.address, 5000, 7],
            box_references=boxes,
            note=b"init2",
        )
    )
    bridge.send.call(
        au.AppClientMethodCallParams(
            method="refund",
            args=[2],
            box_references=[
                au.BoxReference(app_id=0, name=completed_key(2)),
                au.BoxReference(app_id=0, name=refunded_key(2)),
            ],
        )
    )


def test_is_refunded(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(
            method="isTransferRefunded",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=refunded_key(2))],
        )
    )
    assert result.abi_return is True


def test_transfer_count(bridge):
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="getTransferCount")
    )
    assert result.abi_return == 2


def test_pause_and_unpause(bridge):
    bridge.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="isPaused", note=b"p2")
    )
    assert result.abi_return is True

    bridge.send.call(
        au.AppClientMethodCallParams(method="unpause")
    )
    result = bridge.send.call(
        au.AppClientMethodCallParams(method="isPaused", note=b"p3")
    )
    assert result.abi_return is False


def test_set_bridge_fee(bridge):
    bridge.send.call(
        au.AppClientMethodCallParams(
            method="setBridgeFee",
            args=[200],  # 2%
        )
    )
