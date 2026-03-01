"""
Invoicing behavioral tests.
Tests invoice creation, payment, cancellation, and overdue checks.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def payer_key(iid):
    return mapping_box_key("_invoicePayer", iid.to_bytes(64, "big"))


def amount_key(iid):
    return mapping_box_key("_invoiceAmount", iid.to_bytes(64, "big"))


def paid_key(iid):
    return mapping_box_key("_invoicePaid", iid.to_bytes(64, "big"))


def due_key(iid):
    return mapping_box_key("_invoiceDueDate", iid.to_bytes(64, "big"))


def cancelled_key(iid):
    return mapping_box_key("_invoiceCancelled", iid.to_bytes(64, "big"))


def invoice_boxes(iid):
    return [
        au.BoxReference(app_id=0, name=payer_key(iid)),
        au.BoxReference(app_id=0, name=amount_key(iid)),
        au.BoxReference(app_id=0, name=paid_key(iid)),
        au.BoxReference(app_id=0, name=due_key(iid)),
        au.BoxReference(app_id=0, name=cancelled_key(iid)),
    ]


@pytest.fixture(scope="module")
def inv(localnet, account):
    return deploy_contract(localnet, account, "InvoicingTest")


def test_deploy(inv):
    assert inv.app_id > 0


def test_admin(inv, account):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_initial_count(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getInvoiceCount")
    )
    assert result.abi_return == 0


def test_create_invoice(inv, account):
    boxes = invoice_boxes(0)
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="createInvoice",
            args=[account.address, 10000, 5000],  # amount=10000, dueDate=5000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_invoice_count(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getInvoiceCount")
    )
    assert result.abi_return == 1


def test_invoice_payer(inv, account):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoicePayer",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=payer_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_invoice_amount(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoiceAmount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=amount_key(0))],
        )
    )
    assert result.abi_return == 10000


def test_invoice_due_date(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoiceDueDate",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=due_key(0))],
        )
    )
    assert result.abi_return == 5000


def test_invoice_balance(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoiceBalance",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
            ],
        )
    )
    assert result.abi_return == 10000


def test_not_fully_paid(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isInvoiceFullyPaid",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
            ],
        )
    )
    assert result.abi_return is False


def test_not_overdue_early(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isOverdue",
            args=[0, 3000],  # currentTime=3000 < dueDate=5000
            box_references=[
                au.BoxReference(app_id=0, name=due_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
                au.BoxReference(app_id=0, name=cancelled_key(0)),
            ],
        )
    )
    assert result.abi_return is False


def test_overdue(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isOverdue",
            args=[0, 6000],  # currentTime=6000 > dueDate=5000
            box_references=[
                au.BoxReference(app_id=0, name=due_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
                au.BoxReference(app_id=0, name=cancelled_key(0)),
            ],
        )
    )
    assert result.abi_return is True


def test_partial_payment(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="payInvoice",
            args=[0, 4000],
            box_references=[
                au.BoxReference(app_id=0, name=paid_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=cancelled_key(0)),
            ],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoicePaid",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=paid_key(0))],
        )
    )
    assert result.abi_return == 4000


def test_balance_after_partial(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="getInvoiceBalance",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
            ],
            note=b"bal2",
        )
    )
    assert result.abi_return == 6000


def test_full_payment(inv):
    inv.send.call(
        au.AppClientMethodCallParams(
            method="payInvoice",
            args=[0, 6000],
            box_references=[
                au.BoxReference(app_id=0, name=paid_key(0)),
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=cancelled_key(0)),
            ],
            note=b"pay2",
        )
    )


def test_fully_paid(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isInvoiceFullyPaid",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=amount_key(0)),
                au.BoxReference(app_id=0, name=paid_key(0)),
            ],
            note=b"fp2",
        )
    )
    assert result.abi_return is True


def test_total_billed(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getTotalBilled")
    )
    assert result.abi_return == 10000


def test_total_paid(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getTotalPaid")
    )
    assert result.abi_return == 10000


def test_create_and_cancel(inv, account):
    boxes = invoice_boxes(1)
    inv.send.call(
        au.AppClientMethodCallParams(
            method="createInvoice",
            args=[account.address, 8000, 9000],
            box_references=boxes,
            note=b"inv2",
        )
    )
    inv.send.call(
        au.AppClientMethodCallParams(
            method="cancelInvoice",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=cancelled_key(1)),
                au.BoxReference(app_id=0, name=amount_key(1)),
                au.BoxReference(app_id=0, name=paid_key(1)),
            ],
        )
    )
    result = inv.send.call(
        au.AppClientMethodCallParams(
            method="isInvoiceCancelled",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=cancelled_key(1))],
        )
    )
    assert result.abi_return is True


def test_total_cancelled(inv):
    result = inv.send.call(
        au.AppClientMethodCallParams(method="getTotalCancelled")
    )
    assert result.abi_return == 8000
