"""
Fundraiser behavioral tests.
Tests campaign creation, contributions, finalization, and refunds.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def goal_key(cid):
    return mapping_box_key("_campaignGoal", cid.to_bytes(64, "big"))


def raised_key(cid):
    return mapping_box_key("_campaignRaised", cid.to_bytes(64, "big"))


def deadline_key(cid):
    return mapping_box_key("_campaignDeadline", cid.to_bytes(64, "big"))


def finalized_key(cid):
    return mapping_box_key("_campaignFinalized", cid.to_bytes(64, "big"))


def refunded_key(cid):
    return mapping_box_key("_campaignRefunded", cid.to_bytes(64, "big"))


def creator_key(cid):
    return mapping_box_key("_campaignCreator", cid.to_bytes(64, "big"))


def campaign_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=goal_key(cid)),
        au.BoxReference(app_id=0, name=raised_key(cid)),
        au.BoxReference(app_id=0, name=deadline_key(cid)),
        au.BoxReference(app_id=0, name=finalized_key(cid)),
        au.BoxReference(app_id=0, name=refunded_key(cid)),
        au.BoxReference(app_id=0, name=creator_key(cid)),
    ]


@pytest.fixture(scope="module")
def fund(localnet, account):
    return deploy_contract(localnet, account, "FundraiserTest")


def test_deploy(fund):
    assert fund.app_id > 0


def test_admin(fund, account):
    result = fund.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_create_campaign(fund, account):
    boxes = campaign_boxes(0)
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="createCampaign",
            args=[account.address, 10000, 5000],  # goal=10000, deadline=5000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_campaign_count(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(method="getCampaignCount")
    )
    assert result.abi_return == 1


def test_campaign_goal(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="getCampaignGoal",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=goal_key(0))],
        )
    )
    assert result.abi_return == 10000


def test_campaign_creator(fund, account):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="getCampaignCreator",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=creator_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_contribute(fund):
    fund.send.call(
        au.AppClientMethodCallParams(
            method="contribute",
            args=[0, 6000],
            box_references=[
                au.BoxReference(app_id=0, name=raised_key(0)),
                au.BoxReference(app_id=0, name=finalized_key(0)),
                au.BoxReference(app_id=0, name=refunded_key(0)),
            ],
        )
    )


def test_campaign_raised(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="getCampaignRaised",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=raised_key(0))],
        )
    )
    assert result.abi_return == 6000


def test_goal_not_reached(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isGoalReached",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=raised_key(0)),
                au.BoxReference(app_id=0, name=goal_key(0)),
            ],
        )
    )
    assert result.abi_return is False


def test_contribute_more(fund):
    fund.send.call(
        au.AppClientMethodCallParams(
            method="contribute",
            args=[0, 5000],
            box_references=[
                au.BoxReference(app_id=0, name=raised_key(0)),
                au.BoxReference(app_id=0, name=finalized_key(0)),
                au.BoxReference(app_id=0, name=refunded_key(0)),
            ],
            note=b"c2",
        )
    )


def test_goal_reached(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isGoalReached",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=raised_key(0)),
                au.BoxReference(app_id=0, name=goal_key(0)),
            ],
            note=b"gr2",
        )
    )
    assert result.abi_return is True


def test_not_expired(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isExpired",
            args=[0, 3000],
            box_references=[au.BoxReference(app_id=0, name=deadline_key(0))],
        )
    )
    assert result.abi_return is False


def test_expired(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isExpired",
            args=[0, 6000],
            box_references=[au.BoxReference(app_id=0, name=deadline_key(0))],
            note=b"ex2",
        )
    )
    assert result.abi_return is True


def test_finalize(fund):
    fund.send.call(
        au.AppClientMethodCallParams(
            method="finalizeCampaign",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=finalized_key(0))],
        )
    )
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isCampaignFinalized",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=finalized_key(0))],
        )
    )
    assert result.abi_return is True


def test_total_raised(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(method="getTotalRaised")
    )
    assert result.abi_return == 11000


def test_create_and_refund(fund, account):
    boxes = campaign_boxes(1)
    fund.send.call(
        au.AppClientMethodCallParams(
            method="createCampaign",
            args=[account.address, 20000, 8000],
            box_references=boxes,
            note=b"camp2",
        )
    )
    fund.send.call(
        au.AppClientMethodCallParams(
            method="contribute",
            args=[1, 3000],
            box_references=[
                au.BoxReference(app_id=0, name=raised_key(1)),
                au.BoxReference(app_id=0, name=finalized_key(1)),
                au.BoxReference(app_id=0, name=refunded_key(1)),
            ],
            note=b"c3",
        )
    )
    fund.send.call(
        au.AppClientMethodCallParams(
            method="refundCampaign",
            args=[1],
            box_references=[
                au.BoxReference(app_id=0, name=finalized_key(1)),
                au.BoxReference(app_id=0, name=refunded_key(1)),
                au.BoxReference(app_id=0, name=raised_key(1)),
            ],
        )
    )


def test_refunded(fund):
    result = fund.send.call(
        au.AppClientMethodCallParams(
            method="isCampaignRefunded",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=refunded_key(1))],
        )
    )
    assert result.abi_return is True


def test_total_raised_after_refund(fund):
    # 11000 + 3000 - 3000 = 11000
    result = fund.send.call(
        au.AppClientMethodCallParams(method="getTotalRaised", note=b"tr2")
    )
    assert result.abi_return == 11000
