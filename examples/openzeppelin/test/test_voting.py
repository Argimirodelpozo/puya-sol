"""
VotingTest behavioral tests.
Tests proposal creation, voting, execution with nested mappings and structs.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def proposal_box(proposal_id: int) -> bytes:
    pid = proposal_id.to_bytes(64, "big")
    return mapping_box_key("proposals", pid)


def has_voted_box(proposal_id: int, voter_addr: str) -> bytes:
    """Nested mapping: hasVoted[proposalId][voter] = "hasVoted" + sha256(pid || sender)."""
    import hashlib
    pid = proposal_id.to_bytes(64, "big")
    combined = pid + addr_bytes(voter_addr)
    return b"hasVoted" + hashlib.sha256(combined).digest()


@pytest.fixture(scope="module")
def voting(localnet, account):
    return deploy_contract(localnet, account, "VotingTest")


def test_deploy(voting):
    assert voting.app_id > 0


def test_create_proposal(voting, account):
    pb = proposal_box(0)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="createProposal",
            args=[1000],  # deadline
            box_references=[box_ref(voting.app_id, pb)],
        )
    )
    assert result.abi_return == 0  # first proposal ID


def test_get_for_votes(voting):
    pb = proposal_box(0)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getForVotes",
            args=[0],
            box_references=[box_ref(voting.app_id, pb)],
        )
    )
    assert result.abi_return == 0


def test_vote_for(voting, account):
    pb = proposal_box(0)
    hv = has_voted_box(0, account.address)
    voting.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, True],
            box_references=[
                box_ref(voting.app_id, pb),
                box_ref(voting.app_id, hv),
            ],
        )
    )
    # Check forVotes is now 1
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getForVotes",
            args=[0],
            box_references=[box_ref(voting.app_id, pb)],
        )
    )
    assert result.abi_return == 1


def test_already_voted_fails(voting, account):
    pb = proposal_box(0)
    hv = has_voted_box(0, account.address)
    with pytest.raises(Exception):
        voting.send.call(
            au.AppClientMethodCallParams(
                method="vote",
                args=[0, True],
                box_references=[
                    box_ref(voting.app_id, pb),
                    box_ref(voting.app_id, hv),
                ],
            )
        )


def test_execute_proposal(voting):
    """Execute succeeds when forVotes > againstVotes."""
    pb = proposal_box(0)
    # execute should succeed (forVotes=1 > againstVotes=0)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="execute",
            args=[0],
            box_references=[box_ref(voting.app_id, pb)],
        )
    )
    # execute is void — if it didn't revert, it succeeded
    assert result.transaction is not None
