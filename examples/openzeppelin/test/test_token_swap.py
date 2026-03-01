"""
TokenSwap behavioral tests.
Tests depositing, swapping in both directions, quotes, and rate changes.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def bal_a_key(addr):
    return mapping_box_key("_balanceA", encoding.decode_address(addr))


def bal_b_key(addr):
    return mapping_box_key("_balanceB", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def swap(localnet, account):
    return deploy_contract(localnet, account, "TokenSwapTest")


def test_deploy(swap):
    assert swap.app_id > 0


def test_rate(swap):
    result = swap.send.call(
        au.AppClientMethodCallParams(method="rate")
    )
    assert result.abi_return == 2000  # 2.0x


def test_quote_a_to_b(swap):
    result = swap.send.call(
        au.AppClientMethodCallParams(
            method="getQuoteAtoB",
            args=[1000],
        )
    )
    # 1000 * 2000 / 1000 = 2000
    assert result.abi_return == 2000


def test_quote_b_to_a(swap):
    result = swap.send.call(
        au.AppClientMethodCallParams(
            method="getQuoteBtoA",
            args=[2000],
        )
    )
    # 2000 * 1000 / 2000 = 1000
    assert result.abi_return == 1000


def test_deposit_a(swap, account):
    swap.send.call(
        au.AppClientMethodCallParams(
            method="depositA",
            args=[account.address, 10000],
            box_references=[au.BoxReference(app_id=0, name=bal_a_key(account.address))],
        )
    )


def test_balance_a(swap, account):
    result = swap.send.call(
        au.AppClientMethodCallParams(
            method="balanceA",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_a_key(account.address))],
        )
    )
    assert result.abi_return == 10000


def test_swap_a_to_b(swap, account):
    boxes = [
        au.BoxReference(app_id=0, name=bal_a_key(account.address)),
        au.BoxReference(app_id=0, name=bal_b_key(account.address)),
    ]
    result = swap.send.call(
        au.AppClientMethodCallParams(
            method="swapAtoB",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000  # 5000 * 2.0


def test_balances_after_swap(swap, account):
    result_a = swap.send.call(
        au.AppClientMethodCallParams(
            method="balanceA",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_a_key(account.address))],
            note=b"ba2",
        )
    )
    result_b = swap.send.call(
        au.AppClientMethodCallParams(
            method="balanceB",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_b_key(account.address))],
        )
    )
    assert result_a.abi_return == 5000
    assert result_b.abi_return == 10000


def test_total_swapped(swap):
    result = swap.send.call(
        au.AppClientMethodCallParams(method="totalSwapped")
    )
    assert result.abi_return == 5000


def test_swap_b_to_a(swap, account):
    boxes = [
        au.BoxReference(app_id=0, name=bal_a_key(account.address)),
        au.BoxReference(app_id=0, name=bal_b_key(account.address)),
    ]
    result = swap.send.call(
        au.AppClientMethodCallParams(
            method="swapBtoA",
            args=[account.address, 4000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 2000  # 4000 / 2.0


def test_set_rate(swap):
    swap.send.call(
        au.AppClientMethodCallParams(
            method="setRate",
            args=[3000],  # 3.0x
        )
    )
    result = swap.send.call(
        au.AppClientMethodCallParams(method="rate", note=b"r2")
    )
    assert result.abi_return == 3000
