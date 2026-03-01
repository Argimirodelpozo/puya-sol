"""
Ballot behavioral tests.
Tests voting rights, voting, and winner determination.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def voted_key(addr):
    return mapping_box_key("_hasVoted", encoding.decode_address(addr))


def canvote_key(addr):
    return mapping_box_key("_canVote", encoding.decode_address(addr))


def votes_key(pid):
    return mapping_box_key("_proposalVotes", pid.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def ballot(localnet, account):
    return deploy_contract(localnet, account, "BallotTest")


def test_deploy(ballot):
    assert ballot.app_id > 0


def test_chairman(ballot, account):
    result = ballot.send.call(
        au.AppClientMethodCallParams(method="chairman")
    )
    assert result.abi_return == account.address


def test_proposal_count(ballot):
    result = ballot.send.call(
        au.AppClientMethodCallParams(method="proposalCount")
    )
    assert result.abi_return == 3


def test_grant_voting_right(ballot, account):
    boxes = [
        au.BoxReference(app_id=0, name=voted_key(account.address)),
        au.BoxReference(app_id=0, name=canvote_key(account.address)),
    ]
    ballot.send.call(
        au.AppClientMethodCallParams(
            method="grantVotingRight",
            args=[account.address],
            box_references=boxes,
        )
    )


def test_can_vote(ballot, account):
    result = ballot.send.call(
        au.AppClientMethodCallParams(
            method="canVote",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=canvote_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_vote(ballot, account):
    boxes = [
        au.BoxReference(app_id=0, name=canvote_key(account.address)),
        au.BoxReference(app_id=0, name=voted_key(account.address)),
        au.BoxReference(app_id=0, name=votes_key(1)),
    ]
    ballot.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[account.address, 1],  # vote for proposal 1
            box_references=boxes,
        )
    )


def test_has_voted(ballot, account):
    result = ballot.send.call(
        au.AppClientMethodCallParams(
            method="hasVoted",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=voted_key(account.address))],
        )
    )
    assert result.abi_return is True


def test_proposal_votes(ballot):
    result = ballot.send.call(
        au.AppClientMethodCallParams(
            method="proposalVotes",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=votes_key(1))],
        )
    )
    assert result.abi_return == 1


def test_winning_proposal(ballot):
    boxes = [
        au.BoxReference(app_id=0, name=votes_key(0)),
        au.BoxReference(app_id=0, name=votes_key(1)),
        au.BoxReference(app_id=0, name=votes_key(2)),
    ]
    result = ballot.send.call(
        au.AppClientMethodCallParams(
            method="winningProposal",
            box_references=boxes,
        )
    )
    ret = result.abi_return
    vals = list(ret.values()) if isinstance(ret, dict) else list(ret)
    assert vals[0] == 1  # proposal 1 won
    assert vals[1] == 1  # with 1 vote
