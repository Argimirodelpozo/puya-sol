"""
Bounty behavioral tests.
Tests bounty creation, claim submission, approval, and rejection.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def reward_key(bid):
    return mapping_box_key("_bountyReward", bid.to_bytes(64, "big"))


def active_key(bid):
    return mapping_box_key("_bountyActive", bid.to_bytes(64, "big"))


def bclaim_count_key(bid):
    return mapping_box_key("_bountyClaimCount", bid.to_bytes(64, "big"))


def claim_bounty_key(cid):
    return mapping_box_key("_claimBountyId", cid.to_bytes(64, "big"))


def claim_hunter_key(cid):
    return mapping_box_key("_claimHunter", cid.to_bytes(64, "big"))


def claim_status_key(cid):
    return mapping_box_key("_claimStatus", cid.to_bytes(64, "big"))


def bounty_boxes(bid):
    return [
        au.BoxReference(app_id=0, name=reward_key(bid)),
        au.BoxReference(app_id=0, name=active_key(bid)),
        au.BoxReference(app_id=0, name=bclaim_count_key(bid)),
    ]


def claim_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=claim_bounty_key(cid)),
        au.BoxReference(app_id=0, name=claim_hunter_key(cid)),
        au.BoxReference(app_id=0, name=claim_status_key(cid)),
    ]


@pytest.fixture(scope="module")
def bounty(localnet, account):
    return deploy_contract(localnet, account, "BountyTest")


def test_deploy(bounty):
    assert bounty.app_id > 0


def test_admin(bounty, account):
    result = bounty.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_create_bounty(bounty):
    boxes = bounty_boxes(0)
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="createBounty",
            args=[5000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_bounty_count(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(method="getBountyCount")
    )
    assert result.abi_return == 1


def test_bounty_reward(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getBountyReward",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=reward_key(0))],
        )
    )
    assert result.abi_return == 5000


def test_bounty_active(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="isBountyActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    assert result.abi_return is True


def test_total_reward_pool(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(method="getTotalRewardPool")
    )
    assert result.abi_return == 5000


def test_submit_claim(bounty, account):
    cboxes = claim_boxes(0)
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="initClaim",
            args=[0],
            box_references=cboxes,
        )
    )
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="submitClaim",
            args=[0, account.address],
            box_references=cboxes + [
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=bclaim_count_key(0)),
            ],
        )
    )
    assert result.abi_return == 0


def test_claim_count(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(method="getClaimCount")
    )
    assert result.abi_return == 1


def test_claim_bounty_id(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getClaimBountyId",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=claim_bounty_key(0))],
        )
    )
    assert result.abi_return == 0


def test_claim_hunter(bounty, account):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getClaimHunter",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=claim_hunter_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_claim_status_pending(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getClaimStatus",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=claim_status_key(0))],
        )
    )
    assert result.abi_return == 0  # pending


def test_approve_claim(bounty):
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="approveClaim",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=claim_status_key(0)),
                au.BoxReference(app_id=0, name=claim_bounty_key(0)),
                au.BoxReference(app_id=0, name=reward_key(0)),
            ],
        )
    )


def test_claim_status_approved(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getClaimStatus",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=claim_status_key(0))],
            note=b"cs2",
        )
    )
    assert result.abi_return == 1  # approved


def test_total_paid(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(method="getTotalPaid")
    )
    assert result.abi_return == 5000


def test_close_bounty(bounty):
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="closeBounty",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="isBountyActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"ba2",
        )
    )
    assert result.abi_return is False


def test_reopen_bounty(bounty):
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="reopenBounty",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        )
    )
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="isBountyActive",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=active_key(0))],
            note=b"ba3",
        )
    )
    assert result.abi_return is True


def test_submit_and_reject(bounty, account):
    cboxes = claim_boxes(1)
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="initClaim",
            args=[1],
            box_references=cboxes,
        )
    )
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="submitClaim",
            args=[0, account.address],
            box_references=cboxes + [
                au.BoxReference(app_id=0, name=active_key(0)),
                au.BoxReference(app_id=0, name=bclaim_count_key(0)),
            ],
            note=b"sc2",
        )
    )
    bounty.send.call(
        au.AppClientMethodCallParams(
            method="rejectClaim",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=claim_status_key(1))],
        )
    )
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getClaimStatus",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=claim_status_key(1))],
            note=b"cs3",
        )
    )
    assert result.abi_return == 2  # rejected


def test_bounty_claim_count(bounty):
    result = bounty.send.call(
        au.AppClientMethodCallParams(
            method="getBountyClaimCount",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=bclaim_count_key(0))],
        )
    )
    assert result.abi_return == 2
