"""
Referendum behavioral tests.
Tests proposal creation, voting, closing, and pass/fail determination.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def for_key(pid):
    return mapping_box_key("_proposalForVotes", pid.to_bytes(64, "big"))
def against_key(pid):
    return mapping_box_key("_proposalAgainstVotes", pid.to_bytes(64, "big"))
def open_key(pid):
    return mapping_box_key("_proposalOpen", pid.to_bytes(64, "big"))

def proposal_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=for_key(pid)),
        au.BoxReference(app_id=0, name=against_key(pid)),
        au.BoxReference(app_id=0, name=open_key(pid)),
    ]

@pytest.fixture(scope="module")
def ref(localnet, account):
    return deploy_contract(localnet, account, "ReferendumTest")

def test_deploy(ref):
    assert ref.app_id > 0

def test_admin(ref, account):
    r = ref.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_proposal(ref):
    boxes = proposal_boxes(0)
    ref.send.call(au.AppClientMethodCallParams(
        method="initProposal", args=[0], box_references=boxes))
    r = ref.send.call(au.AppClientMethodCallParams(
        method="createProposal", box_references=boxes))
    assert r.abi_return == 0

def test_proposal_count(ref):
    r = ref.send.call(au.AppClientMethodCallParams(method="getProposalCount"))
    assert r.abi_return == 1

def test_proposal_open(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="isProposalOpen", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))]))
    assert r.abi_return is True

def test_vote_for(ref):
    for i in range(5):
        ref.send.call(au.AppClientMethodCallParams(
            method="voteFor", args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=open_key(0)),
                au.BoxReference(app_id=0, name=for_key(0)),
            ], note=f"vf{i}".encode()))

def test_vote_against(ref):
    for i in range(3):
        ref.send.call(au.AppClientMethodCallParams(
            method="voteAgainst", args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=open_key(0)),
                au.BoxReference(app_id=0, name=against_key(0)),
            ], note=f"va{i}".encode()))

def test_for_votes(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="getForVotes", args=[0],
        box_references=[au.BoxReference(app_id=0, name=for_key(0))]))
    assert r.abi_return == 5

def test_against_votes(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="getAgainstVotes", args=[0],
        box_references=[au.BoxReference(app_id=0, name=against_key(0))]))
    assert r.abi_return == 3

def test_total_participation(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="getTotalParticipation", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=for_key(0)),
            au.BoxReference(app_id=0, name=against_key(0)),
        ]))
    assert r.abi_return == 8

def test_close_proposal(ref):
    ref.send.call(au.AppClientMethodCallParams(
        method="closeProposal", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))]))
    r = ref.send.call(au.AppClientMethodCallParams(
        method="isProposalOpen", args=[0],
        box_references=[au.BoxReference(app_id=0, name=open_key(0))],
        note=b"o2"))
    assert r.abi_return is False

def test_proposal_passed(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="isProposalPassed", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=for_key(0)),
            au.BoxReference(app_id=0, name=against_key(0)),
        ]))
    assert r.abi_return is True

def test_create_failing_proposal(ref):
    boxes = proposal_boxes(1)
    ref.send.call(au.AppClientMethodCallParams(
        method="initProposal", args=[1], box_references=boxes))
    ref.send.call(au.AppClientMethodCallParams(
        method="createProposal", box_references=boxes, note=b"c2"))
    # 2 for, 5 against → fails
    for i in range(2):
        ref.send.call(au.AppClientMethodCallParams(
            method="voteFor", args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=open_key(1)),
                au.BoxReference(app_id=0, name=for_key(1)),
            ], note=f"f2_{i}".encode()))
    for i in range(5):
        ref.send.call(au.AppClientMethodCallParams(
            method="voteAgainst", args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=open_key(1)),
                au.BoxReference(app_id=0, name=against_key(1)),
            ], note=f"a2_{i}".encode()))
    ref.send.call(au.AppClientMethodCallParams(
        method="closeProposal", args=[1],
        box_references=[au.BoxReference(app_id=0, name=open_key(1))]))

def test_failing_proposal_not_passed(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="isProposalPassed", args=[1],
        box_references=[
            au.BoxReference(app_id=0, name=for_key(1)),
            au.BoxReference(app_id=0, name=against_key(1)),
        ]))
    assert r.abi_return is False

def test_count_after(ref):
    r = ref.send.call(au.AppClientMethodCallParams(
        method="getProposalCount", note=b"c3"))
    assert r.abi_return == 2
