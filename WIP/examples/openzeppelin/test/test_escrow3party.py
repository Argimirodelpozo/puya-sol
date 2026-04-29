"""
Escrow3Party behavioral tests.
Tests deal creation, funding, release, refund, and dispute flows.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def buyer_key(did):
    return mapping_box_key("_dealBuyer", did.to_bytes(64, "big"))


def seller_key(did):
    return mapping_box_key("_dealSeller", did.to_bytes(64, "big"))


def arbiter_key(did):
    return mapping_box_key("_dealArbiter", did.to_bytes(64, "big"))


def amount_key(did):
    return mapping_box_key("_dealAmount", did.to_bytes(64, "big"))


def status_key(did):
    return mapping_box_key("_dealStatus", did.to_bytes(64, "big"))


def deal_boxes(did):
    return [
        au.BoxReference(app_id=0, name=buyer_key(did)),
        au.BoxReference(app_id=0, name=seller_key(did)),
        au.BoxReference(app_id=0, name=arbiter_key(did)),
        au.BoxReference(app_id=0, name=amount_key(did)),
        au.BoxReference(app_id=0, name=status_key(did)),
    ]


@pytest.fixture(scope="module")
def esc(localnet, account):
    return deploy_contract(localnet, account, "Escrow3PartyTest")


def test_deploy(esc):
    assert esc.app_id > 0


def test_admin(esc, account):
    result = esc.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_create_deal(esc, account):
    boxes = deal_boxes(0)
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="createDeal",
            args=[account.address, account.address, account.address, 10000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_deal_count(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(method="dealCount")
    )
    assert result.abi_return == 1


def test_deal_amount(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealAmount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
        )
    )
    assert result.abi_return == 10000


def test_deal_status_created(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=status_key(0))],
        )
    )
    assert result.abi_return == 0  # created


def test_fund_deal(esc):
    esc.send.call(
        au.AppClientMethodCallParams(
            method="fundDeal",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=status_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
            ],
        )
    )


def test_status_funded(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=status_key(0))],
            note=b"s2",
        )
    )
    assert result.abi_return == 1  # funded


def test_total_escrowed(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(method="totalEscrowed")
    )
    assert result.abi_return == 10000


def test_release_deal(esc):
    esc.send.call(
        au.AppClientMethodCallParams(
            method="releaseDeal",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=status_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
            ],
        )
    )


def test_status_released(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=status_key(0))],
            note=b"s3",
        )
    )
    assert result.abi_return == 2  # released


def test_total_released(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(method="totalReleased")
    )
    assert result.abi_return == 10000


def test_create_second_deal(esc, account):
    boxes = deal_boxes(1)
    esc.send.call(
        au.AppClientMethodCallParams(
            method="createDeal",
            args=[account.address, account.address, account.address, 5000],
            box_references=boxes,
            note=b"d2",
        )
    )


def test_fund_and_dispute(esc):
    esc.send.call(
        au.AppClientMethodCallParams(
            method="fundDeal",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=status_key(1)),
                au.BoxReference(app_id=0, name=amount_key(1)),
            ],
            note=b"f2",
        )
    )
    esc.send.call(
        au.AppClientMethodCallParams(
            method="disputeDeal",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=status_key(1))],
        )
    )


def test_status_disputed(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=status_key(1))],
        )
    )
    assert result.abi_return == 4  # disputed


def test_refund_disputed(esc):
    esc.send.call(
        au.AppClientMethodCallParams(
            method="refundDeal",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=status_key(1)),
                au.BoxReference(app_id=0, name=amount_key(1)),
            ],
        )
    )
    result = esc.send.call(
        au.AppClientMethodCallParams(
            method="getDealStatus",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=status_key(1))],
            note=b"s4",
        )
    )
    assert result.abi_return == 3  # refunded


def test_total_refunded(esc):
    result = esc.send.call(
        au.AppClientMethodCallParams(method="totalRefunded")
    )
    assert result.abi_return == 5000
