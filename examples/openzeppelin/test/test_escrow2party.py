"""
Escrow2Party behavioral tests.
Tests deal creation, release, refund, dispute, and resolution.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def buyer_key(did):
    return mapping_box_key("_dealBuyer", did.to_bytes(64, "big"))


def seller_key(did):
    return mapping_box_key("_dealSeller", did.to_bytes(64, "big"))


def amount_key(did):
    return mapping_box_key("_dealAmount", did.to_bytes(64, "big"))


def status_key(did):
    return mapping_box_key("_dealStatus", did.to_bytes(64, "big"))


def deal_boxes(did):
    return [
        au.BoxReference(app_id=0, name=buyer_key(did)),
        au.BoxReference(app_id=0, name=seller_key(did)),
        au.BoxReference(app_id=0, name=amount_key(did)),
        au.BoxReference(app_id=0, name=status_key(did)),
    ]


@pytest.fixture(scope="module")
def escrow2(localnet, account):
    return deploy_contract(localnet, account, "Escrow2PartyTest")


def test_deploy(escrow2):
    assert escrow2.app_id > 0


def test_arbiter(escrow2, account):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(method="arbiter")
    )
    assert result.abi_return == account.address


def test_fee_rate(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(method="feeRate")
    )
    assert result.abi_return == 100


def test_create_deal(escrow2, account):
    boxes = deal_boxes(1)
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="createDeal",
            args=[account.address, account.address, 5000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 1


def test_deal_amount(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="getDealAmount",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=amount_key(1))],
        )
    )
    assert result.abi_return == 5000


def test_deal_status_pending(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=status_key(1))],
        )
    )
    assert result.abi_return == 0  # pending


def test_release_funds(escrow2):
    boxes = [
        au.BoxReference(app_id=0, name=status_key(1)),
        au.BoxReference(app_id=0, name=amount_key(1)),
    ]
    escrow2.send.call(
        au.AppClientMethodCallParams(
            method="releaseFunds",
            args=[1],
            box_references=boxes,
        )
    )


def test_status_after_release(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=status_key(1))],
            note=b"s2",
        )
    )
    assert result.abi_return == 1  # released


def test_fees_collected(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(method="feesCollected")
    )
    # 5000 * 100 / 10000 = 50
    assert result.abi_return == 50


def test_create_deal2(escrow2, account):
    boxes = deal_boxes(2)
    escrow2.send.call(
        au.AppClientMethodCallParams(
            method="createDeal",
            args=[account.address, account.address, 3000],
            box_references=boxes,
        )
    )


def test_dispute(escrow2):
    escrow2.send.call(
        au.AppClientMethodCallParams(
            method="dispute",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=status_key(2))],
        )
    )


def test_status_disputed(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=status_key(2))],
            note=b"s3",
        )
    )
    assert result.abi_return == 3  # disputed


def test_resolve_dispute(escrow2):
    boxes = [
        au.BoxReference(app_id=0, name=status_key(2)),
        au.BoxReference(app_id=0, name=amount_key(2)),
    ]
    escrow2.send.call(
        au.AppClientMethodCallParams(
            method="resolveDispute",
            args=[2, False],  # refund to buyer
            box_references=boxes,
        )
    )


def test_status_refunded(escrow2):
    result = escrow2.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=status_key(2))],
            note=b"s4",
        )
    )
    assert result.abi_return == 2  # refunded
