"""
Marketplace behavioral tests.
Tests item listing, buying, fee calculation, and cancellation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def seller_key(item_id):
    return mapping_box_key("_itemSeller", item_id.to_bytes(64, "big"))


def price_key(item_id):
    return mapping_box_key("_itemPrice", item_id.to_bytes(64, "big"))


def sold_key(item_id):
    return mapping_box_key("_itemSold", item_id.to_bytes(64, "big"))


def item_boxes(item_id):
    return [
        au.BoxReference(app_id=0, name=seller_key(item_id)),
        au.BoxReference(app_id=0, name=price_key(item_id)),
        au.BoxReference(app_id=0, name=sold_key(item_id)),
    ]


@pytest.fixture(scope="module")
def market(localnet, account):
    return deploy_contract(localnet, account, "MarketplaceTest")


def test_deploy(market):
    assert market.app_id > 0


def test_owner(market, account):
    result = market.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_fee(market):
    result = market.send.call(
        au.AppClientMethodCallParams(method="feePercent")
    )
    assert result.abi_return == 250


def test_initial_item_count(market):
    result = market.send.call(
        au.AppClientMethodCallParams(method="itemCount")
    )
    assert result.abi_return == 0


def test_list_item(market, account):
    boxes = item_boxes(1)
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="listItem",
            args=[account.address, 10000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 1  # item ID


def test_item_count_after_list(market):
    result = market.send.call(
        au.AppClientMethodCallParams(method="itemCount", note=b"ic1")
    )
    assert result.abi_return == 1


def test_item_seller(market, account):
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="getItemSeller",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=seller_key(1))],
        )
    )
    assert result.abi_return == account.address


def test_item_price(market):
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="getItemPrice",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=price_key(1))],
        )
    )
    assert result.abi_return == 10000


def test_calculate_fee(market):
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="calculateFee",
            args=[10000],
        )
    )
    # 10000 * 250 / 10000 = 250
    assert result.abi_return == 250


def test_item_not_sold(market):
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="isItemSold",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=sold_key(1))],
        )
    )
    assert result.abi_return is False


def test_buy_item(market):
    boxes = item_boxes(1)
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="buyItem",
            args=[1],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_item_sold_after_buy(market):
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="isItemSold",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=sold_key(1))],
            note=b"sold2",
        )
    )
    assert result.abi_return is True


def test_total_fees_after_buy(market):
    result = market.send.call(
        au.AppClientMethodCallParams(method="totalFees")
    )
    assert result.abi_return == 250


def test_list_second_item(market, account):
    boxes = item_boxes(2)
    result = market.send.call(
        au.AppClientMethodCallParams(
            method="listItem",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 2


def test_cancel_item(market, account):
    boxes = [
        au.BoxReference(app_id=0, name=seller_key(2)),
        au.BoxReference(app_id=0, name=sold_key(2)),
        au.BoxReference(app_id=0, name=price_key(2)),
    ]
    market.send.call(
        au.AppClientMethodCallParams(
            method="cancelItem",
            args=[2, account.address],
            box_references=boxes,
        )
    )


def test_set_fee(market):
    market.send.call(
        au.AppClientMethodCallParams(
            method="setFeePercent",
            args=[500],  # 5%
        )
    )
    result = market.send.call(
        au.AppClientMethodCallParams(method="feePercent", note=b"fp2")
    )
    assert result.abi_return == 500
