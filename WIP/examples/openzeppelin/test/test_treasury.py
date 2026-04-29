"""
Treasury behavioral tests.
Tests deposit, proposal creation, approval, and execution.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def creator_key(pid):
    return mapping_box_key("_proposalCreator", pid.to_bytes(64, "big"))


def amount_key(pid):
    return mapping_box_key("_proposalAmount", pid.to_bytes(64, "big"))


def recipient_key(pid):
    return mapping_box_key("_proposalRecipient", pid.to_bytes(64, "big"))


def approvals_key(pid):
    return mapping_box_key("_proposalApprovals", pid.to_bytes(64, "big"))


def executed_key(pid):
    return mapping_box_key("_proposalExecuted", pid.to_bytes(64, "big"))


def proposal_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=creator_key(pid)),
        au.BoxReference(app_id=0, name=amount_key(pid)),
        au.BoxReference(app_id=0, name=recipient_key(pid)),
        au.BoxReference(app_id=0, name=approvals_key(pid)),
        au.BoxReference(app_id=0, name=executed_key(pid)),
    ]


@pytest.fixture(scope="module")
def treasury(localnet, account):
    return deploy_contract(localnet, account, "TreasuryTest")


def test_deploy(treasury):
    assert treasury.app_id > 0


def test_admin(treasury, account):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_initial_required_approvals(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="requiredApprovals")
    )
    assert result.abi_return == 2


def test_initial_funds(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="totalFunds")
    )
    assert result.abi_return == 0


def test_deposit(treasury):
    treasury.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[10000],
        )
    )


def test_funds_after_deposit(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="totalFunds", note=b"f1")
    )
    assert result.abi_return == 10000


def test_create_proposal(treasury, account):
    boxes = proposal_boxes(1)
    result = treasury.send.call(
        au.AppClientMethodCallParams(
            method="createProposal",
            args=[account.address, 3000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 1  # proposal ID


def test_proposal_count(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="proposalCount")
    )
    assert result.abi_return == 1


def test_proposal_amount(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(
            method="getProposalAmount",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=amount_key(1))],
        )
    )
    assert result.abi_return == 3000


def test_approve_first(treasury):
    boxes = [
        au.BoxReference(app_id=0, name=creator_key(1)),
        au.BoxReference(app_id=0, name=executed_key(1)),
        au.BoxReference(app_id=0, name=approvals_key(1)),
    ]
    treasury.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[1],
            box_references=boxes,
        )
    )


def test_approve_second(treasury):
    boxes = [
        au.BoxReference(app_id=0, name=creator_key(1)),
        au.BoxReference(app_id=0, name=executed_key(1)),
        au.BoxReference(app_id=0, name=approvals_key(1)),
    ]
    treasury.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[1],
            box_references=boxes,
            note=b"approve2",
        )
    )


def test_approvals_count(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(
            method="getProposalApprovals",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=approvals_key(1))],
        )
    )
    assert result.abi_return == 2


def test_execute_proposal(treasury):
    boxes = [
        au.BoxReference(app_id=0, name=creator_key(1)),
        au.BoxReference(app_id=0, name=executed_key(1)),
        au.BoxReference(app_id=0, name=approvals_key(1)),
        au.BoxReference(app_id=0, name=amount_key(1)),
    ]
    result = treasury.send.call(
        au.AppClientMethodCallParams(
            method="executeProposal",
            args=[1],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_executed_flag(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(
            method="isProposalExecuted",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=executed_key(1))],
        )
    )
    assert result.abi_return is True


def test_funds_after_execution(treasury):
    result = treasury.send.call(
        au.AppClientMethodCallParams(method="totalFunds", note=b"f2")
    )
    assert result.abi_return == 7000
