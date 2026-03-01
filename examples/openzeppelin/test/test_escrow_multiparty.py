"""
EscrowMultiParty behavioral tests.
Tests multi-party escrow with arbiter resolution, voting, and deadlines.
"""

import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding
from Crypto.Hash import keccak


@pytest.fixture(scope="module")
def escrow(localnet, account):
    return deploy_contract(localnet, account, "EscrowMultiParty")


@pytest.fixture(scope="module")
def accounts(localnet, account):
    """Create beneficiary and arbiter accounts."""
    beneficiary = localnet.account.random()
    arbiter = localnet.account.random()
    fund_account(localnet, account, beneficiary)
    fund_account(localnet, account, arbiter)
    return {"depositor": account, "beneficiary": beneficiary, "arbiter": arbiter}


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def escrow_id_bytes(eid: int) -> bytes:
    return eid.to_bytes(64, "big")


def vote_key(escrow_id: int, voter_addr: str) -> bytes:
    """Compute keccak256(abi.encodePacked(escrowId, voter)) box key."""
    eid_bytes = escrow_id.to_bytes(64, "big")
    voter_bytes = encoding.decode_address(voter_addr)
    h = keccak.new(digest_bits=256)
    h.update(eid_bytes + voter_bytes)
    keccak_result = h.digest()
    return mapping_box_key("_hasVoted", keccak_result)


# --- Deployment ---

def test_deploy(escrow):
    assert escrow.app_id > 0


def test_initial_count(escrow):
    result = escrow.send.call(au.AppClientMethodCallParams(method="escrowCount"))
    assert result.abi_return == 0


# --- Create escrow ---

def test_create_escrow(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    amt_key = mapping_box_key("_amounts", eid)
    st_key = mapping_box_key("_states", eid)
    dl_key = mapping_box_key("_deadlines", eid)

    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="createEscrow",
            args=[
                addr_bytes(accounts["beneficiary"].address),
                addr_bytes(accounts["arbiter"].address),
                1000,
                9999999999,
            ],
            box_references=[
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, amt_key),
                box_ref(app_id, st_key),
                box_ref(app_id, dl_key),
            ],
        )
    )
    assert result.abi_return == 0  # first escrow ID


def test_escrow_count_after_create(escrow):
    result = escrow.send.call(au.AppClientMethodCallParams(method="escrowCount"))
    assert result.abi_return == 1


def test_get_depositor(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    dep_key = mapping_box_key("_depositors", eid)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getDepositor",
            args=[0],
            box_references=[box_ref(app_id, dep_key)],
        )
    )
    assert result.abi_return == accounts["depositor"].address


def test_get_beneficiary(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    ben_key = mapping_box_key("_beneficiaries", eid)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getBeneficiary",
            args=[0],
            box_references=[box_ref(app_id, ben_key)],
        )
    )
    assert result.abi_return == accounts["beneficiary"].address


def test_get_amount(escrow):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    amt_key = mapping_box_key("_amounts", eid)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getAmount",
            args=[0],
            box_references=[box_ref(app_id, amt_key)],
        )
    )
    assert result.abi_return == 1000


def test_get_state_created(escrow):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getState",
            args=[0],
            box_references=[box_ref(app_id, st_key)],
        )
    )
    assert result.abi_return == 0  # State.Created


# --- Fund escrow ---

def test_fund_escrow(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    amt_key = mapping_box_key("_amounts", eid)

    escrow.send.call(
        au.AppClientMethodCallParams(
            method="fund",
            args=[0],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, amt_key),
            ],
        )
    )

    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getState",
            args=[0],
            box_references=[box_ref(app_id, st_key)],
        )
    )
    assert result.abi_return == 1  # State.Funded


# --- Dispute ---

def test_dispute_escrow(escrow, accounts, localnet):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)

    # Beneficiary disputes
    ben_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=escrow.app_spec,
            app_id=escrow.app_id,
            default_sender=accounts["beneficiary"].address,
        )
    )
    ben_client.send.call(
        au.AppClientMethodCallParams(
            method="dispute",
            args=[0],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
            ],
        )
    )

    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getState",
            args=[0],
            box_references=[box_ref(app_id, st_key)],
        )
    )
    assert result.abi_return == 2  # State.Disputed


# --- Voting ---

def test_depositor_votes_refund(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    vk = vote_key(0, accounts["depositor"].address)
    vr_key = mapping_box_key("_votesRefund", eid)
    vrel_key = mapping_box_key("_votesRelease", eid)

    escrow.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, False],  # vote to refund
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, vk),
                box_ref(app_id, vr_key),
                box_ref(app_id, vrel_key),
            ],
        )
    )


def test_has_voted(escrow, accounts):
    app_id = escrow.app_id
    vk = vote_key(0, accounts["depositor"].address)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="hasVoted",
            args=[0, addr_bytes(accounts["depositor"].address)],
            box_references=[box_ref(app_id, vk)],
        )
    )
    assert result.abi_return is True


def test_cannot_vote_twice(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    vk = vote_key(0, accounts["depositor"].address)
    vr_key = mapping_box_key("_votesRefund", eid)
    vrel_key = mapping_box_key("_votesRelease", eid)

    with pytest.raises(Exception):
        escrow.send.call(
            au.AppClientMethodCallParams(
                method="vote",
                args=[0, False],
                box_references=[
                    box_ref(app_id, st_key),
                    box_ref(app_id, dep_key),
                    box_ref(app_id, ben_key),
                    box_ref(app_id, arb_key),
                    box_ref(app_id, vk),
                    box_ref(app_id, vr_key),
                    box_ref(app_id, vrel_key),
                ],
            )
        )


def test_arbiter_votes_refund_auto_resolves(escrow, accounts, localnet):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    vk = vote_key(0, accounts["arbiter"].address)
    vr_key = mapping_box_key("_votesRefund", eid)
    vrel_key = mapping_box_key("_votesRelease", eid)

    # Arbiter votes refund (2nd refund vote = auto-resolve)
    arb_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=escrow.app_spec,
            app_id=escrow.app_id,
            default_sender=accounts["arbiter"].address,
        )
    )
    arb_client.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, False],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, vk),
                box_ref(app_id, vr_key),
                box_ref(app_id, vrel_key),
            ],
        )
    )

    # Should auto-resolve to Refunded (state=4)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getState",
            args=[0],
            box_references=[box_ref(app_id, st_key)],
        )
    )
    assert result.abi_return == 4  # State.Refunded


def test_get_votes(escrow):
    app_id = escrow.app_id
    eid = escrow_id_bytes(0)
    vr_key = mapping_box_key("_votesRefund", eid)
    vrel_key = mapping_box_key("_votesRelease", eid)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getVotes",
            args=[0],
            box_references=[
                box_ref(app_id, vrel_key),
                box_ref(app_id, vr_key),
            ],
        )
    )
    # Tuple return has named fields from ARC56 spec
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["release"] == 0
        assert ret["refund"] == 2
    else:
        assert ret == [0, 2]


# --- Second escrow: test release resolution ---

def test_create_second_escrow(escrow, accounts):
    app_id = escrow.app_id
    eid = escrow_id_bytes(1)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    amt_key = mapping_box_key("_amounts", eid)
    st_key = mapping_box_key("_states", eid)
    dl_key = mapping_box_key("_deadlines", eid)

    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="createEscrow",
            args=[
                addr_bytes(accounts["beneficiary"].address),
                addr_bytes(accounts["arbiter"].address),
                500,
                9999999999,
            ],
            box_references=[
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, amt_key),
                box_ref(app_id, st_key),
                box_ref(app_id, dl_key),
            ],
        )
    )
    assert result.abi_return == 1


def test_fund_and_dispute_second(escrow, accounts, localnet):
    app_id = escrow.app_id
    eid = escrow_id_bytes(1)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    amt_key = mapping_box_key("_amounts", eid)

    # Fund
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="fund",
            args=[1],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, amt_key),
            ],
        )
    )

    # Dispute by depositor
    escrow.send.call(
        au.AppClientMethodCallParams(
            method="dispute",
            args=[1],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
            ],
        )
    )


def test_release_resolution(escrow, accounts, localnet):
    app_id = escrow.app_id
    eid = escrow_id_bytes(1)
    st_key = mapping_box_key("_states", eid)
    dep_key = mapping_box_key("_depositors", eid)
    ben_key = mapping_box_key("_beneficiaries", eid)
    arb_key = mapping_box_key("_arbiters", eid)
    vrel_key = mapping_box_key("_votesRelease", eid)
    vr_key = mapping_box_key("_votesRefund", eid)

    # Beneficiary votes release
    ben_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=escrow.app_spec,
            app_id=escrow.app_id,
            default_sender=accounts["beneficiary"].address,
        )
    )
    vk_ben = vote_key(1, accounts["beneficiary"].address)
    ben_client.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[1, True],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, vk_ben),
                box_ref(app_id, vr_key),
                box_ref(app_id, vrel_key),
            ],
        )
    )

    # Arbiter votes release (2nd release vote = auto-resolve)
    arb_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=escrow.app_spec,
            app_id=escrow.app_id,
            default_sender=accounts["arbiter"].address,
        )
    )
    vk_arb = vote_key(1, accounts["arbiter"].address)
    arb_client.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[1, True],
            box_references=[
                box_ref(app_id, st_key),
                box_ref(app_id, dep_key),
                box_ref(app_id, ben_key),
                box_ref(app_id, arb_key),
                box_ref(app_id, vk_arb),
                box_ref(app_id, vr_key),
                box_ref(app_id, vrel_key),
            ],
        )
    )

    # Should auto-resolve to Resolved (state=3)
    result = escrow.send.call(
        au.AppClientMethodCallParams(
            method="getState",
            args=[1],
            box_references=[box_ref(app_id, st_key)],
        )
    )
    assert result.abi_return == 3  # State.Resolved
