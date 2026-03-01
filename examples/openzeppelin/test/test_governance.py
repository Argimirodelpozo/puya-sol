"""
Governance behavioral tests.
Tests proposal creation, voting (for/against/abstain), quorum, and execution.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def vp_key(addr):
    return mapping_box_key("_votingPower", encoding.decode_address(addr))


def for_key(pid):
    return mapping_box_key("_forVotes", pid.to_bytes(64, "big"))


def against_key(pid):
    return mapping_box_key("_againstVotes", pid.to_bytes(64, "big"))


def abstain_key(pid):
    return mapping_box_key("_abstainVotes", pid.to_bytes(64, "big"))


def executed_key(pid):
    return mapping_box_key("_proposalExecuted", pid.to_bytes(64, "big"))


def deadline_key(pid):
    return mapping_box_key("_proposalDeadline", pid.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def gov(localnet, account):
    return deploy_contract(localnet, account, "GovernanceTest")


def test_deploy(gov):
    assert gov.app_id > 0


def test_admin(gov, account):
    result = gov.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_quorum(gov):
    result = gov.send.call(
        au.AppClientMethodCallParams(method="quorum")
    )
    assert result.abi_return == 10


def test_set_voting_power(gov, account):
    gov.send.call(
        au.AppClientMethodCallParams(
            method="setVotingPower",
            args=[account.address, 15],
            box_references=[au.BoxReference(app_id=0, name=vp_key(account.address))],
        )
    )


def test_create_proposal(gov):
    boxes = [au.BoxReference(app_id=0, name=deadline_key(1))]
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="createProposal",
            args=[1000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 1


def test_proposal_deadline(gov):
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="proposalDeadline",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=deadline_key(1))],
        )
    )
    assert result.abi_return == 1100  # 1000 + 100 voting period


def test_cast_vote_for(gov, account):
    boxes = [
        au.BoxReference(app_id=0, name=vp_key(account.address)),
        au.BoxReference(app_id=0, name=for_key(1)),
        au.BoxReference(app_id=0, name=against_key(1)),
        au.BoxReference(app_id=0, name=abstain_key(1)),
    ]
    gov.send.call(
        au.AppClientMethodCallParams(
            method="castVote",
            args=[1, 1, account.address],  # proposal 1, support=1 (for)
            box_references=boxes,
        )
    )


def test_for_votes(gov):
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="forVotes",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=for_key(1))],
        )
    )
    assert result.abi_return == 15


def test_proposal_passed(gov):
    boxes = [
        au.BoxReference(app_id=0, name=for_key(1)),
        au.BoxReference(app_id=0, name=against_key(1)),
        au.BoxReference(app_id=0, name=abstain_key(1)),
    ]
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="isProposalPassed",
            args=[1],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_execute_proposal(gov):
    boxes = [
        au.BoxReference(app_id=0, name=executed_key(1)),
        au.BoxReference(app_id=0, name=deadline_key(1)),
        au.BoxReference(app_id=0, name=for_key(1)),
        au.BoxReference(app_id=0, name=against_key(1)),
        au.BoxReference(app_id=0, name=abstain_key(1)),
    ]
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="executeProposal",
            args=[1, 1200],  # past deadline
            box_references=boxes,
        )
    )
    assert result.abi_return is True


def test_executed(gov):
    result = gov.send.call(
        au.AppClientMethodCallParams(
            method="isProposalExecuted",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=executed_key(1))],
        )
    )
    assert result.abi_return is True
