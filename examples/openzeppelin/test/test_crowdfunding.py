"""
Crowdfunding behavioral tests.
Tests pledging, unpledging, claiming, and refunding.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def pledge_key(addr):
    return mapping_box_key("_pledges", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def crowd(localnet, account):
    return deploy_contract(localnet, account, "CrowdfundingTest")


def test_deploy(crowd):
    assert crowd.app_id > 0


def test_creator(crowd, account):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="creator")
    )
    assert result.abi_return == account.address


def test_goal(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="goal")
    )
    assert result.abi_return == 10000


def test_initial_pledged(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="totalPledged")
    )
    assert result.abi_return == 0


def test_pledge(crowd, account):
    crowd.send.call(
        au.AppClientMethodCallParams(
            method="pledge",
            args=[account.address, 6000, 100],
            box_references=[au.BoxReference(app_id=0, name=pledge_key(account.address))],
        )
    )


def test_pledge_of(crowd, account):
    result = crowd.send.call(
        au.AppClientMethodCallParams(
            method="pledgeOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=pledge_key(account.address))],
        )
    )
    assert result.abi_return == 6000


def test_backer_count(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="backerCount")
    )
    assert result.abi_return == 1


def test_not_goal_reached(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="isGoalReached")
    )
    assert result.abi_return is False


def test_pledge_more(crowd, account):
    crowd.send.call(
        au.AppClientMethodCallParams(
            method="pledge",
            args=[account.address, 5000, 200],
            box_references=[au.BoxReference(app_id=0, name=pledge_key(account.address))],
            note=b"pledge2",
        )
    )


def test_goal_reached(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="isGoalReached", note=b"gr2")
    )
    assert result.abi_return is True


def test_unpledge(crowd, account):
    crowd.send.call(
        au.AppClientMethodCallParams(
            method="unpledge",
            args=[account.address, 1000],
            box_references=[au.BoxReference(app_id=0, name=pledge_key(account.address))],
        )
    )


def test_pledged_after_unpledge(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="totalPledged", note=b"tp2")
    )
    assert result.abi_return == 10000


def test_claim(crowd):
    crowd.send.call(
        au.AppClientMethodCallParams(method="claim")
    )


def test_claimed(crowd):
    result = crowd.send.call(
        au.AppClientMethodCallParams(method="claimed")
    )
    assert result.abi_return is True
