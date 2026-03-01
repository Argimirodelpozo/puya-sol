"""
Insurance behavioral tests.
Tests policy creation, activation, claims, and cancellation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def holder_key(pid):
    return mapping_box_key("_policyHolder", pid.to_bytes(64, "big"))


def coverage_key(pid):
    return mapping_box_key("_policyCoverage", pid.to_bytes(64, "big"))


def premium_key(pid):
    return mapping_box_key("_policyPremium", pid.to_bytes(64, "big"))


def active_key(pid):
    return mapping_box_key("_policyActive", pid.to_bytes(64, "big"))


def claimed_key(pid):
    return mapping_box_key("_policyClaimed", pid.to_bytes(64, "big"))


def policy_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=holder_key(pid)),
        au.BoxReference(app_id=0, name=coverage_key(pid)),
        au.BoxReference(app_id=0, name=premium_key(pid)),
        au.BoxReference(app_id=0, name=active_key(pid)),
        au.BoxReference(app_id=0, name=claimed_key(pid)),
    ]


@pytest.fixture(scope="module")
def ins(localnet, account):
    return deploy_contract(localnet, account, "InsuranceTest")


def test_deploy(ins):
    assert ins.app_id > 0


def test_admin(ins, account):
    result = ins.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_create_policy(ins, account):
    boxes = policy_boxes(1)
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="createPolicy",
            args=[account.address, 10000],  # coverage=10000
            box_references=boxes,
        )
    )
    assert result.abi_return == 1


def test_policy_holder(ins, account):
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="getPolicyHolder",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=holder_key(1))],
        )
    )
    assert result.abi_return == account.address


def test_policy_coverage(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="getPolicyCoverage",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=coverage_key(1))],
        )
    )
    assert result.abi_return == 10000


def test_policy_premium(ins):
    # premium = 10000 * 50 / 1000 = 500
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="getPolicyPremium",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=premium_key(1))],
        )
    )
    assert result.abi_return == 500


def test_policy_count(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(method="getPolicyCount")
    )
    assert result.abi_return == 1


def test_activate_policy(ins):
    ins.send.call(
        au.AppClientMethodCallParams(
            method="activatePolicy",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(1)),
                au.BoxReference(app_id=0, name=premium_key(1)),
            ],
        )
    )


def test_is_active(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="isPolicyActive",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=active_key(1))],
        )
    )
    assert result.abi_return is True


def test_total_premiums(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(method="getTotalPremiums")
    )
    assert result.abi_return == 500


def test_file_claim(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="fileClaim",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(1)),
                au.BoxReference(app_id=0, name=claimed_key(1)),
                au.BoxReference(app_id=0, name=coverage_key(1)),
            ],
        )
    )
    assert result.abi_return == 10000


def test_is_claimed(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="isPolicyClaimed",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=claimed_key(1))],
        )
    )
    assert result.abi_return is True


def test_total_payouts(ins):
    result = ins.send.call(
        au.AppClientMethodCallParams(method="getTotalPayouts")
    )
    assert result.abi_return == 10000


def test_create_and_cancel(ins, account):
    boxes = policy_boxes(2)
    ins.send.call(
        au.AppClientMethodCallParams(
            method="createPolicy",
            args=[account.address, 5000],
            box_references=boxes,
            note=b"pol2",
        )
    )
    ins.send.call(
        au.AppClientMethodCallParams(
            method="activatePolicy",
            args=[2],
            box_references=[
                au.BoxReference(app_id=0, name=active_key(2)),
                au.BoxReference(app_id=0, name=premium_key(2)),
            ],
        )
    )
    ins.send.call(
        au.AppClientMethodCallParams(
            method="cancelPolicy",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=active_key(2))],
        )
    )
    result = ins.send.call(
        au.AppClientMethodCallParams(
            method="isPolicyActive",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=active_key(2))],
            note=b"act2",
        )
    )
    assert result.abi_return is False
