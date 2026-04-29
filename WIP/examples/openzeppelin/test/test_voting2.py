"""
Voting2 behavioral tests.
Tests voter registration, poll creation, weighted voting, and finalization.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def weight_key(addr):
    return mapping_box_key("_voterWeight", encoding.decode_address(addr))


def voter_idx_key(addr):
    return mapping_box_key("_voterIndex", encoding.decode_address(addr))


def quorum_key(pid):
    return mapping_box_key("_pollQuorum", pid.to_bytes(64, "big"))


def yes_key(pid):
    return mapping_box_key("_pollYesVotes", pid.to_bytes(64, "big"))


def no_key(pid):
    return mapping_box_key("_pollNoVotes", pid.to_bytes(64, "big"))


def finalized_key(pid):
    return mapping_box_key("_pollFinalized", pid.to_bytes(64, "big"))


def passed_key(pid):
    return mapping_box_key("_pollPassed", pid.to_bytes(64, "big"))


def voter_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=weight_key(addr)),
        au.BoxReference(app_id=0, name=voter_idx_key(addr)),
    ]


def poll_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=quorum_key(pid)),
        au.BoxReference(app_id=0, name=yes_key(pid)),
        au.BoxReference(app_id=0, name=no_key(pid)),
        au.BoxReference(app_id=0, name=finalized_key(pid)),
        au.BoxReference(app_id=0, name=passed_key(pid)),
    ]


@pytest.fixture(scope="module")
def voting(localnet, account):
    return deploy_contract(localnet, account, "Voting2Test")


def test_deploy(voting):
    assert voting.app_id > 0


def test_admin(voting, account):
    result = voting.send.call(
        au.AppClientMethodCallParams(method="admin")
    )
    assert result.abi_return == account.address


def test_register_voter(voting, account):
    boxes = voter_boxes(account.address)
    voting.send.call(
        au.AppClientMethodCallParams(
            method="initVoter",
            args=[account.address],
            box_references=boxes,
        )
    )
    voting.send.call(
        au.AppClientMethodCallParams(
            method="registerVoter",
            args=[account.address, 10],
            box_references=boxes,
        )
    )


def test_voter_count(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(method="voterCount")
    )
    assert result.abi_return == 1


def test_voter_weight(voting, account):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="voterWeight",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=weight_key(account.address))],
        )
    )
    assert result.abi_return == 10


def test_create_poll(voting):
    boxes = poll_boxes(0)
    voting.send.call(
        au.AppClientMethodCallParams(
            method="initPoll",
            args=[0],
            box_references=boxes,
        )
    )
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="createPoll",
            args=[15],  # quorum=15
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_poll_count(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(method="pollCount")
    )
    assert result.abi_return == 1


def test_poll_quorum(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="pollQuorum",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=quorum_key(0))],
        )
    )
    assert result.abi_return == 15


def test_vote_yes(voting, account):
    voting.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, account.address, True],
            box_references=[
                au.BoxReference(app_id=0, name=finalized_key(0)),
                au.BoxReference(app_id=0, name=weight_key(account.address)),
                au.BoxReference(app_id=0, name=yes_key(0)),
                au.BoxReference(app_id=0, name=no_key(0)),
            ],
        )
    )


def test_yes_votes(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="pollYesVotes",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=yes_key(0))],
        )
    )
    assert result.abi_return == 10


def test_total_votes_cast(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(method="totalVotesCast")
    )
    assert result.abi_return == 1


def test_not_finalized(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="pollFinalized",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=finalized_key(0))],
        )
    )
    assert result.abi_return is False


def test_vote_no_with_second_voter(voting, account):
    # Vote again with same account as no (weight=10) — total becomes 20 >= quorum 15
    voting.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, account.address, False],
            box_references=[
                au.BoxReference(app_id=0, name=finalized_key(0)),
                au.BoxReference(app_id=0, name=weight_key(account.address)),
                au.BoxReference(app_id=0, name=yes_key(0)),
                au.BoxReference(app_id=0, name=no_key(0)),
            ],
            note=b"v2",
        )
    )


def test_no_votes(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="pollNoVotes",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=no_key(0))],
        )
    )
    assert result.abi_return == 10


def test_finalize_poll(voting):
    # yes=10, no=10, total=20 >= quorum=15
    voting.send.call(
        au.AppClientMethodCallParams(
            method="finalizePoll",
            args=[0],
            box_references=poll_boxes(0),
        )
    )


def test_finalized(voting):
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="pollFinalized",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=finalized_key(0))],
            note=b"f2",
        )
    )
    assert result.abi_return is True


def test_poll_result(voting):
    # yes=10, no=10 → not passed (requires yes > no, not >=)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getPollResult",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=finalized_key(0)),
                au.BoxReference(app_id=0, name=passed_key(0)),
            ],
        )
    )
    assert result.abi_return is False  # tie → not passed
