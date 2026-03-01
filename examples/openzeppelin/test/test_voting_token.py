"""
VotingToken behavioral tests.
Tests mint/burn, proposal creation, voting, and execution.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def bal_key(addr):
    return mapping_box_key("_balances", encoding.decode_address(addr))


def for_key(pid):
    return mapping_box_key("_propForVotes", pid.to_bytes(64, "big"))


def against_key(pid):
    return mapping_box_key("_propAgainstVotes", pid.to_bytes(64, "big"))


def start_key(pid):
    return mapping_box_key("_propStartTime", pid.to_bytes(64, "big"))


def end_key(pid):
    return mapping_box_key("_propEndTime", pid.to_bytes(64, "big"))


def exec_key(pid):
    return mapping_box_key("_propExecuted", pid.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def vt(localnet, account):
    return deploy_contract(localnet, account, "VotingTokenTest")


def test_deploy(vt):
    assert vt.app_id > 0


def test_admin(vt, account):
    result = vt.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_mint(vt, account):
    vt.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1000],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
        )
    )


def test_balance(vt, account):
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
        )
    )
    assert result.abi_return == 1000


def test_total_supply(vt):
    result = vt.send.call(
        au.AppClientMethodCallParams(method="getTotalSupply")
    )
    assert result.abi_return == 1000


def test_burn(vt, account):
    vt.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 200],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
        )
    )
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
            note=b"bal2",
        )
    )
    assert result.abi_return == 800


def test_create_proposal(vt):
    boxes = [
        au.BoxReference(app_id=0, name=start_key(0)),
        au.BoxReference(app_id=0, name=end_key(0)),
        au.BoxReference(app_id=0, name=exec_key(0)),
        au.BoxReference(app_id=0, name=for_key(0)),
        au.BoxReference(app_id=0, name=against_key(0)),
    ]
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="createProposal",
            args=[5000],  # startTime=5000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0  # 0-indexed


def test_vote_for(vt, account):
    vt.send.call(
        au.AppClientMethodCallParams(
            method="voteFor",
            args=[0, account.address],
            box_references=[
                au.BoxReference(app_id=0, name=for_key(0)),
                au.BoxReference(app_id=0, name=bal_key(account.address)),
            ],
        )
    )


def test_for_votes(vt):
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="getForVotes",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=for_key(0))],
        )
    )
    assert result.abi_return == 800


def test_proposal_passed(vt):
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="isProposalPassed",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=for_key(0)),
                au.BoxReference(app_id=0, name=against_key(0)),
            ],
        )
    )
    assert result.abi_return is True


def test_execute_proposal(vt):
    vt.send.call(
        au.AppClientMethodCallParams(
            method="executeProposal",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=exec_key(0)),
                au.BoxReference(app_id=0, name=for_key(0)),
                au.BoxReference(app_id=0, name=against_key(0)),
            ],
        )
    )


def test_is_executed(vt):
    result = vt.send.call(
        au.AppClientMethodCallParams(
            method="isExecuted",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=exec_key(0))],
        )
    )
    assert result.abi_return is True
