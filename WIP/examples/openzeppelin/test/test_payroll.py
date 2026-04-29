"""
Payroll behavioral tests.
Tests employee management, payments, termination, and salary updates.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def salary_key(addr):
    return mapping_box_key("_salary", encoding.decode_address(addr))


def paid_key(addr):
    return mapping_box_key("_totalPaid", encoding.decode_address(addr))


def payment_count_key(addr):
    return mapping_box_key("_paymentCount", encoding.decode_address(addr))


def employee_key(addr):
    return mapping_box_key("_isEmployee", encoding.decode_address(addr))


def terminated_key(addr):
    return mapping_box_key("_isTerminated", encoding.decode_address(addr))


def emp_idx_key(addr):
    return mapping_box_key("_employeeIndex", encoding.decode_address(addr))


def emp_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=salary_key(addr)),
        au.BoxReference(app_id=0, name=paid_key(addr)),
        au.BoxReference(app_id=0, name=payment_count_key(addr)),
        au.BoxReference(app_id=0, name=employee_key(addr)),
        au.BoxReference(app_id=0, name=terminated_key(addr)),
        au.BoxReference(app_id=0, name=emp_idx_key(addr)),
    ]


@pytest.fixture(scope="module")
def pay(localnet, account):
    return deploy_contract(localnet, account, "PayrollTest")


def test_deploy(pay):
    assert pay.app_id > 0


def test_admin(pay, account):
    result = pay.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_init_and_add(pay, account):
    boxes = emp_boxes(account.address)
    pay.send.call(
        au.AppClientMethodCallParams(
            method="initEmployee",
            args=[account.address],
            box_references=boxes,
        )
    )
    pay.send.call(
        au.AppClientMethodCallParams(
            method="addEmployee",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )


def test_is_employee(pay, account):
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="isEmployee",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=employee_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_salary(pay, account):
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="getSalary",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=salary_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_employee_count(pay):
    result = pay.send.call(
        au.AppClientMethodCallParams(method="employeeCount")
    )
    assert result.abi_return == 1


def test_total_payroll(pay):
    result = pay.send.call(
        au.AppClientMethodCallParams(method="totalPayroll")
    )
    assert result.abi_return == 5000


def test_pay_employee(pay, account):
    pay.send.call(
        au.AppClientMethodCallParams(
            method="payEmployee",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=employee_key(account.address)),
                au.BoxReference(app_id=0, name=terminated_key(account.address)),
                au.BoxReference(app_id=0, name=salary_key(account.address)),
                au.BoxReference(app_id=0, name=paid_key(account.address)),
                au.BoxReference(app_id=0, name=payment_count_key(account.address)),
            ],
        )
    )


def test_total_paid_to(pay, account):
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="getTotalPaidTo",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=paid_key(account.address))],
        )
    )
    assert result.abi_return == 5000


def test_payment_count(pay, account):
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="getPaymentCount",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=payment_count_key(account.address))],
        )
    )
    assert result.abi_return == 1


def test_total_paid_out(pay):
    result = pay.send.call(
        au.AppClientMethodCallParams(method="totalPaidOut")
    )
    assert result.abi_return == 5000


def test_pay_again(pay, account):
    pay.send.call(
        au.AppClientMethodCallParams(
            method="payEmployee",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=employee_key(account.address)),
                au.BoxReference(app_id=0, name=terminated_key(account.address)),
                au.BoxReference(app_id=0, name=salary_key(account.address)),
                au.BoxReference(app_id=0, name=paid_key(account.address)),
                au.BoxReference(app_id=0, name=payment_count_key(account.address)),
            ],
            note=b"p2",
        )
    )
    result = pay.send.call(
        au.AppClientMethodCallParams(method="totalPaidOut", note=b"tpo2")
    )
    assert result.abi_return == 10000


def test_update_salary(pay, account):
    pay.send.call(
        au.AppClientMethodCallParams(
            method="updateSalary",
            args=[account.address, 7000],
            box_references=[
                au.BoxReference(app_id=0, name=employee_key(account.address)),
                au.BoxReference(app_id=0, name=terminated_key(account.address)),
                au.BoxReference(app_id=0, name=salary_key(account.address)),
            ],
        )
    )
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="getSalary",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=salary_key(account.address))],
            note=b"s2",
        )
    )
    assert result.abi_return == 7000


def test_total_payroll_after_update(pay):
    # Was 5000, now 7000
    result = pay.send.call(
        au.AppClientMethodCallParams(method="totalPayroll", note=b"tp2")
    )
    assert result.abi_return == 7000


def test_terminate(pay, account):
    pay.send.call(
        au.AppClientMethodCallParams(
            method="terminate",
            args=[account.address],
            box_references=[
                au.BoxReference(app_id=0, name=employee_key(account.address)),
                au.BoxReference(app_id=0, name=terminated_key(account.address)),
                au.BoxReference(app_id=0, name=salary_key(account.address)),
            ],
        )
    )
    result = pay.send.call(
        au.AppClientMethodCallParams(
            method="isTerminated",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=terminated_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_total_payroll_after_terminate(pay):
    result = pay.send.call(
        au.AppClientMethodCallParams(method="totalPayroll", note=b"tp3")
    )
    assert result.abi_return == 0
