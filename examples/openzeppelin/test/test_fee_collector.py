"""
FeeCollector behavioral tests.
Tests fee collection, custom rates, withdrawal.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def custom_rate_key(addr):
    return mapping_box_key("_customFeeRate", encoding.decode_address(addr))


def fees_paid_key(addr):
    return mapping_box_key("_feesPaid", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def collector(localnet, account):
    return deploy_contract(localnet, account, "FeeCollectorTest")


def test_deploy(collector):
    assert collector.app_id > 0


def test_owner(collector, account):
    result = collector.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_default_rate(collector):
    result = collector.send.call(
        au.AppClientMethodCallParams(method="defaultFeeRate")
    )
    assert result.abi_return == 300


def test_calculate_fee(collector, account):
    boxes = [au.BoxReference(app_id=0, name=custom_rate_key(account.address))]
    result = collector.send.call(
        au.AppClientMethodCallParams(
            method="calculateFee",
            args=[account.address, 10000],
            box_references=boxes,
        )
    )
    # 10000 * 300 / 10000 = 300
    assert result.abi_return == 300


def test_collect_fee(collector, account):
    boxes = [
        au.BoxReference(app_id=0, name=custom_rate_key(account.address)),
        au.BoxReference(app_id=0, name=fees_paid_key(account.address)),
    ]
    result = collector.send.call(
        au.AppClientMethodCallParams(
            method="collectFee",
            args=[account.address, 10000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 300


def test_total_collected(collector):
    result = collector.send.call(
        au.AppClientMethodCallParams(method="totalCollected")
    )
    assert result.abi_return == 300


def test_fees_paid(collector, account):
    result = collector.send.call(
        au.AppClientMethodCallParams(
            method="feesPaid",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=fees_paid_key(account.address))],
        )
    )
    assert result.abi_return == 300


def test_set_custom_rate(collector, account):
    collector.send.call(
        au.AppClientMethodCallParams(
            method="setCustomFeeRate",
            args=[account.address, 100],  # 1%
            box_references=[au.BoxReference(app_id=0, name=custom_rate_key(account.address))],
        )
    )


def test_fee_with_custom_rate(collector, account):
    boxes = [au.BoxReference(app_id=0, name=custom_rate_key(account.address))]
    result = collector.send.call(
        au.AppClientMethodCallParams(
            method="calculateFee",
            args=[account.address, 10000],
            box_references=boxes,
            note=b"calc2",
        )
    )
    assert result.abi_return == 100  # 1% of 10000


def test_withdraw(collector):
    collector.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[150],
        )
    )


def test_available_balance(collector):
    result = collector.send.call(
        au.AppClientMethodCallParams(method="availableBalance")
    )
    assert result.abi_return == 150  # 300 - 150
