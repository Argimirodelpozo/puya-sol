"""
FlashLoan behavioral tests.
Tests deposit, loan initiation, repayment, and fee calculations.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def dep_key(addr):
    return mapping_box_key("_deposited", encoding.decode_address(addr))


def loan_amount_key(lid):
    return mapping_box_key("_loanAmount", lid.to_bytes(64, "big"))


def loan_borrower_key(lid):
    return mapping_box_key("_loanBorrower", lid.to_bytes(64, "big"))


def loan_repaid_key(lid):
    return mapping_box_key("_loanRepaid", lid.to_bytes(64, "big"))


def loan_boxes(lid):
    return [
        au.BoxReference(app_id=0, name=loan_amount_key(lid)),
        au.BoxReference(app_id=0, name=loan_borrower_key(lid)),
        au.BoxReference(app_id=0, name=loan_repaid_key(lid)),
    ]


@pytest.fixture(scope="module")
def fl(localnet, account):
    return deploy_contract(localnet, account, "FlashLoanTest")


def test_deploy(fl):
    assert fl.app_id > 0


def test_admin(fl, account):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_fee_rate(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getFeeRate")
    )
    assert result.abi_return == 30


def test_calculate_fee(fl):
    # 100000 * 30 / 10000 = 300
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="calculateFee",
            args=[100000],
        )
    )
    assert result.abi_return == 300


def test_deposit(fl, account):
    fl.send.call(
        au.AppClientMethodCallParams(
            method="initProvider",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    fl.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[account.address, 100000],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )


def test_deposited(fl, account):
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="getDeposited",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    assert result.abi_return == 100000


def test_total_pool(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getTotalPool")
    )
    assert result.abi_return == 100000


def test_initiate_loan(fl, account):
    boxes = loan_boxes(0)
    fl.send.call(
        au.AppClientMethodCallParams(
            method="initLoan",
            args=[0],
            box_references=boxes,
        )
    )
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="initiateLoan",
            args=[account.address, 50000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_loan_amount(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="getLoanAmount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=loan_amount_key(0))],
        )
    )
    assert result.abi_return == 50000


def test_loan_borrower(fl, account):
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="getLoanBorrower",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=loan_borrower_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_pool_after_loan(fl):
    # 100000 - 50000 = 50000
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getTotalPool", note=b"tp2")
    )
    assert result.abi_return == 50000


def test_active_loan_count(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getActiveLoanCount")
    )
    assert result.abi_return == 1


def test_repay_loan(fl):
    # fee = 50000 * 30 / 10000 = 150
    # must repay >= 50000 + 150 = 50150
    fl.send.call(
        au.AppClientMethodCallParams(
            method="repayLoan",
            args=[0, 50150],
            box_references=[
                au.BoxReference(app_id=0, name=loan_amount_key(0)),
                au.BoxReference(app_id=0, name=loan_repaid_key(0)),
            ],
        )
    )


def test_loan_repaid(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="isLoanRepaid",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=loan_repaid_key(0))],
        )
    )
    assert result.abi_return is True


def test_pool_after_repay(fl):
    # 50000 + 50150 = 100150
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getTotalPool", note=b"tp3")
    )
    assert result.abi_return == 100150


def test_fees_earned(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getTotalFeesEarned")
    )
    assert result.abi_return == 150


def test_active_after_repay(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getActiveLoanCount", note=b"ac2")
    )
    assert result.abi_return == 0


def test_withdraw(fl, account):
    fl.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[account.address, 30000],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
        )
    )
    result = fl.send.call(
        au.AppClientMethodCallParams(
            method="getDeposited",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=dep_key(account.address))],
            note=b"dep2",
        )
    )
    assert result.abi_return == 70000


def test_set_fee_rate(fl):
    fl.send.call(
        au.AppClientMethodCallParams(
            method="setFeeRate",
            args=[50],
        )
    )
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getFeeRate", note=b"fr2")
    )
    assert result.abi_return == 50


def test_loan_count(fl):
    result = fl.send.call(
        au.AppClientMethodCallParams(method="getLoanCount")
    )
    assert result.abi_return == 1
