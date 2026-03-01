"""
MultiSig behavioral tests.
Tests proposal creation, approval, execution, and cancellation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def approver_key(addr):
    return mapping_box_key("_isApprover", encoding.decode_address(addr))


def amount_key(pid):
    return mapping_box_key("_propAmount", pid.to_bytes(64, "big"))


def recipient_key(pid):
    return mapping_box_key("_propRecipient", pid.to_bytes(64, "big"))


def approval_count_key(pid):
    return mapping_box_key("_propApprovalCount", pid.to_bytes(64, "big"))


def executed_key(pid):
    return mapping_box_key("_propExecuted", pid.to_bytes(64, "big"))


def cancelled_key(pid):
    return mapping_box_key("_propCancelled", pid.to_bytes(64, "big"))


def proposal_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=amount_key(pid)),
        au.BoxReference(app_id=0, name=recipient_key(pid)),
        au.BoxReference(app_id=0, name=approval_count_key(pid)),
        au.BoxReference(app_id=0, name=executed_key(pid)),
        au.BoxReference(app_id=0, name=cancelled_key(pid)),
    ]


@pytest.fixture(scope="module")
def msig(localnet, account):
    return deploy_contract(localnet, account, "MultiSigTest")


def test_deploy(msig):
    assert msig.app_id > 0


def test_owner(msig, account):
    result = msig.send.call(
        au.AppClientMethodCallParams(method="getOwner")
    )
    assert result.abi_return == account.address


def test_add_approver(msig, account):
    msig.send.call(
        au.AppClientMethodCallParams(
            method="initApprover",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=approver_key(account.address))],
        )
    )
    msig.send.call(
        au.AppClientMethodCallParams(
            method="addApprover",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=approver_key(account.address))],
        )
    )


def test_is_approver(msig, account):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="isApprover",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=approver_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_propose(msig, account):
    boxes = proposal_boxes(1)
    msig.send.call(
        au.AppClientMethodCallParams(
            method="initProposal",
            args=[1],
            box_references=boxes,
        )
    )
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="propose",
            args=[account.address, 5000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 1


def test_proposal_amount(msig):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="getProposalAmount",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=amount_key(1))],
        )
    )
    assert result.abi_return == 5000


def test_proposal_recipient(msig, account):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="getProposalRecipient",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=recipient_key(1))],
        )
    )
    assert result.abi_return == account.address


def test_approve_first(msig):
    msig.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=approval_count_key(1)),
                au.BoxReference(app_id=0, name=executed_key(1)),
                au.BoxReference(app_id=0, name=cancelled_key(1)),
            ],
        )
    )


def test_approval_count_1(msig):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="getApprovalCount",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=approval_count_key(1))],
        )
    )
    assert result.abi_return == 1


def test_approve_second(msig):
    msig.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=approval_count_key(1)),
                au.BoxReference(app_id=0, name=executed_key(1)),
                au.BoxReference(app_id=0, name=cancelled_key(1)),
            ],
            note=b"approve2",
        )
    )


def test_execute_proposal(msig):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="executeProposal",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=approval_count_key(1)),
                au.BoxReference(app_id=0, name=executed_key(1)),
                au.BoxReference(app_id=0, name=cancelled_key(1)),
            ],
        )
    )
    assert result.abi_return is True


def test_is_executed(msig):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="isExecuted",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=executed_key(1))],
        )
    )
    assert result.abi_return is True


def test_propose_and_cancel(msig, account):
    boxes = proposal_boxes(2)
    msig.send.call(
        au.AppClientMethodCallParams(
            method="initProposal",
            args=[2],
            box_references=boxes,
        )
    )
    msig.send.call(
        au.AppClientMethodCallParams(
            method="propose",
            args=[account.address, 3000],
            box_references=boxes,
            note=b"prop2",
        )
    )
    msig.send.call(
        au.AppClientMethodCallParams(
            method="cancelProposal",
            args=[2],
            box_references=[
                au.BoxReference(app_id=0, name=executed_key(2)),
                au.BoxReference(app_id=0, name=cancelled_key(2)),
            ],
        )
    )


def test_is_cancelled(msig):
    result = msig.send.call(
        au.AppClientMethodCallParams(
            method="isCancelled",
            args=[2],
            box_references=[au.BoxReference(app_id=0, name=cancelled_key(2))],
        )
    )
    assert result.abi_return is True
