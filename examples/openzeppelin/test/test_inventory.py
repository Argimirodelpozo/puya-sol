"""
Inventory behavioral tests.
Tests item management, restocking, selling, and low stock detection.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def quantity_key(iid):
    return mapping_box_key("_itemQuantity", iid.to_bytes(64, "big"))


def price_key(iid):
    return mapping_box_key("_itemPrice", iid.to_bytes(64, "big"))


def active_key(iid):
    return mapping_box_key("_itemActive", iid.to_bytes(64, "big"))


def sold_key(iid):
    return mapping_box_key("_itemSold", iid.to_bytes(64, "big"))


def item_boxes(iid):
    return [
        au.BoxReference(app_id=0, name=quantity_key(iid)),
        au.BoxReference(app_id=0, name=price_key(iid)),
        au.BoxReference(app_id=0, name=active_key(iid)),
        au.BoxReference(app_id=0, name=sold_key(iid)),
    ]


@pytest.fixture(scope="module")
def inv(localnet, account):
    return deploy_contract(localnet, account, "InventoryTest")


def test_deploy(inv):
    assert inv.app_id > 0


def test_admin(inv, account):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_low_stock_threshold(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getLowStockThreshold")
    )
    assert result.abi_return == 5


def test_add_item(inv):
    boxes = item_boxes(0)
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="addItem",
            args=[20, 100],  # quantity=20, price=100
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_item_count(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getItemCount")
    )
    assert result.abi_return == 1


def test_item_quantity(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemQuantity",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quantity_key(0))],
        )
    )
    assert result.abi_return == 20


def test_item_price(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemPrice",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=price_key(0))],
        )
    )
    assert result.abi_return == 100


def test_item_active(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isItemActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    assert result.abi_return is True


def test_item_value(inv):
    # 20 * 100 = 2000
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemValue",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=quantity_key(0)),
                au.BoxReference(app_id=0, name=price_key(0)),
            ],
        )
    )
    assert result.abi_return == 2000


def test_not_low_stock(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isLowStock",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quantity_key(0))],
        )
    )
    assert result.abi_return is False


def test_total_items(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getTotalItems")
    )
    assert result.abi_return == 20


def test_sell(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="sell",
            args=[0, 8],
            box_references=[
                au.BoxReference(app_id=0, name=quantity_key(0)),
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=sold_key(0)),
            ],
        )
    )


def test_quantity_after_sell(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemQuantity",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quantity_key(0))],
            note=b"q2",
        )
    )
    assert result.abi_return == 12


def test_sold(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemSold",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=sold_key(0))],
        )
    )
    assert result.abi_return == 8


def test_restock(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="restock",
            args=[0, 10],
            box_references=[
                au.BoxReference(app_id=0, name=quantity_key(0)),
                au.BoxReference(app_id=0, name=active_key(0)),
            ],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemQuantity",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quantity_key(0))],
            note=b"q3",
        )
    )
    assert result.abi_return == 22


def test_sell_to_low_stock(inv):
    # Sell 18, leaving 4 (below threshold 5)
    inv.send.call(
        au.AppClientMethodCallParams(
            method="sell",
            args=[0, 18],
            box_references=[
                au.BoxReference(app_id=0, name=quantity_key(0)),
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=sold_key(0)),
            ],
            note=b"s2",
        )
    )


def test_is_low_stock(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isLowStock",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quantity_key(0))],
            note=b"ls2",
        )
    )
    assert result.abi_return is True


def test_discontinue(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="discontinue",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isItemActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"a2",
        )
    )
    assert result.abi_return is False


def test_reactivate(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="reactivate",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isItemActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"a3",
        )
    )
    assert result.abi_return is True


def test_set_price(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="setPrice",
            args=[0, 200],
            box_references=[au.BoxReference(app_id=0, name=price_key(0))],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getItemPrice",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=price_key(0))],
            note=b"p2",
        )
    )
    assert result.abi_return == 200


def test_add_second_item(inv):
    boxes = item_boxes(1)
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="addItem",
            args=[50, 30],
            box_references=boxes,
            note=b"i2",
        )
    )
    assert result.abi_return == 1


def test_set_threshold(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="setLowStockThreshold",
            args=[10],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getLowStockThreshold", note=b"th2")
    )
    assert result.abi_return == 10
