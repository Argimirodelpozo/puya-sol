"""
Voting3 behavioral tests.
Tests multi-option elections, voting, and winner determination.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def options_key(eid):
    return mapping_box_key("_electionOptions", eid.to_bytes(64, "big"))
def votes0_key(eid):
    return mapping_box_key("_electionVotesOpt0", eid.to_bytes(64, "big"))
def votes1_key(eid):
    return mapping_box_key("_electionVotesOpt1", eid.to_bytes(64, "big"))
def votes2_key(eid):
    return mapping_box_key("_electionVotesOpt2", eid.to_bytes(64, "big"))
def votes3_key(eid):
    return mapping_box_key("_electionVotesOpt3", eid.to_bytes(64, "big"))
def closed_key(eid):
    return mapping_box_key("_electionClosed", eid.to_bytes(64, "big"))
def total_votes_key(eid):
    return mapping_box_key("_electionTotalVotes", eid.to_bytes(64, "big"))

def election_boxes(eid):
    return [
        au.BoxReference(app_id=0, name=options_key(eid)),
        au.BoxReference(app_id=0, name=votes0_key(eid)),
        au.BoxReference(app_id=0, name=votes1_key(eid)),
        au.BoxReference(app_id=0, name=votes2_key(eid)),
        au.BoxReference(app_id=0, name=votes3_key(eid)),
        au.BoxReference(app_id=0, name=closed_key(eid)),
        au.BoxReference(app_id=0, name=total_votes_key(eid)),
    ]

@pytest.fixture(scope="module")
def v3(localnet, account):
    return deploy_contract(localnet, account, "Voting3Test")

def test_deploy(v3):
    assert v3.app_id > 0

def test_admin(v3, account):
    r = v3.send.call(au.AppClientMethodCallParams(method="admin"))
    assert r.abi_return == account.address

def test_create_election(v3):
    boxes = election_boxes(0)
    v3.send.call(au.AppClientMethodCallParams(
        method="initElection", args=[0], box_references=boxes))
    r = v3.send.call(au.AppClientMethodCallParams(
        method="createElection", args=[3], box_references=boxes))  # 3 options
    assert r.abi_return == 0

def test_election_count(v3):
    r = v3.send.call(au.AppClientMethodCallParams(method="electionCount"))
    assert r.abi_return == 1

def test_election_options(v3):
    r = v3.send.call(au.AppClientMethodCallParams(
        method="electionOptions", args=[0],
        box_references=[au.BoxReference(app_id=0, name=options_key(0))]))
    assert r.abi_return == 3

def test_cast_votes(v3):
    # 3 votes for option 0, 2 for option 1, 5 for option 2
    for i in range(3):
        v3.send.call(au.AppClientMethodCallParams(
            method="castVote", args=[0, 0],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=options_key(0)),
                au.BoxReference(app_id=0, name=votes0_key(0)),
                au.BoxReference(app_id=0, name=votes1_key(0)),
                au.BoxReference(app_id=0, name=votes2_key(0)),
                au.BoxReference(app_id=0, name=votes3_key(0)),
                au.BoxReference(app_id=0, name=total_votes_key(0)),
            ], note=f"v0_{i}".encode()))
    for i in range(2):
        v3.send.call(au.AppClientMethodCallParams(
            method="castVote", args=[0, 1],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=options_key(0)),
                au.BoxReference(app_id=0, name=votes0_key(0)),
                au.BoxReference(app_id=0, name=votes1_key(0)),
                au.BoxReference(app_id=0, name=votes2_key(0)),
                au.BoxReference(app_id=0, name=votes3_key(0)),
                au.BoxReference(app_id=0, name=total_votes_key(0)),
            ], note=f"v1_{i}".encode()))
    for i in range(5):
        v3.send.call(au.AppClientMethodCallParams(
            method="castVote", args=[0, 2],
            box_references=[
                au.BoxReference(app_id=0, name=closed_key(0)),
                au.BoxReference(app_id=0, name=options_key(0)),
                au.BoxReference(app_id=0, name=votes0_key(0)),
                au.BoxReference(app_id=0, name=votes1_key(0)),
                au.BoxReference(app_id=0, name=votes2_key(0)),
                au.BoxReference(app_id=0, name=votes3_key(0)),
                au.BoxReference(app_id=0, name=total_votes_key(0)),
            ], note=f"v2_{i}".encode()))

def test_votes_opt0(v3):
    r = v3.send.call(au.AppClientMethodCallParams(
        method="getVotesForOption", args=[0, 0],
        box_references=[
            au.BoxReference(app_id=0, name=options_key(0)),
            au.BoxReference(app_id=0, name=votes0_key(0)),
            au.BoxReference(app_id=0, name=votes1_key(0)),
            au.BoxReference(app_id=0, name=votes2_key(0)),
            au.BoxReference(app_id=0, name=votes3_key(0)),
        ]))
    assert r.abi_return == 3

def test_votes_opt2(v3):
    r = v3.send.call(au.AppClientMethodCallParams(
        method="getVotesForOption", args=[0, 2],
        box_references=[
            au.BoxReference(app_id=0, name=options_key(0)),
            au.BoxReference(app_id=0, name=votes0_key(0)),
            au.BoxReference(app_id=0, name=votes1_key(0)),
            au.BoxReference(app_id=0, name=votes2_key(0)),
            au.BoxReference(app_id=0, name=votes3_key(0)),
        ]))
    assert r.abi_return == 5

def test_total_votes(v3):
    r = v3.send.call(au.AppClientMethodCallParams(
        method="electionTotalVotes", args=[0],
        box_references=[au.BoxReference(app_id=0, name=total_votes_key(0))]))
    assert r.abi_return == 10

def test_close_election(v3):
    v3.send.call(au.AppClientMethodCallParams(
        method="closeElection", args=[0],
        box_references=[au.BoxReference(app_id=0, name=closed_key(0))]))

def test_winning_option(v3):
    r = v3.send.call(au.AppClientMethodCallParams(
        method="getWinningOption", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=options_key(0)),
            au.BoxReference(app_id=0, name=votes0_key(0)),
            au.BoxReference(app_id=0, name=votes1_key(0)),
            au.BoxReference(app_id=0, name=votes2_key(0)),
            au.BoxReference(app_id=0, name=votes3_key(0)),
        ]))
    assert r.abi_return == 2  # option 2 had 5 votes

def test_total_votes_cast(v3):
    r = v3.send.call(au.AppClientMethodCallParams(method="totalVotesCast"))
    assert r.abi_return == 10
