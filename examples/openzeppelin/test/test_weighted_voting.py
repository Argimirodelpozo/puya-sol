"""
WeightedVoting behavioral tests.
Tests weighted voting with delegation, proposal lifecycle, and auto-finalization.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding
from Crypto.Hash import keccak


@pytest.fixture(scope="module")
def voting(localnet, account):
    return deploy_contract(localnet, account, "WeightedVoting")


@pytest.fixture(scope="module")
def voters(localnet, account):
    """Create voter accounts."""
    v1 = localnet.account.random()
    v2 = localnet.account.random()
    v3 = localnet.account.random()
    fund_account(localnet, account, v1)
    fund_account(localnet, account, v2)
    fund_account(localnet, account, v3)
    return {"admin": account, "v1": v1, "v2": v2, "v3": v3}


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def proposal_id_bytes(pid: int) -> bytes:
    return pid.to_bytes(64, "big")


def vote_box_key(proposal_id: int, voter_addr: str) -> bytes:
    """Compute box key for _hasVoted mapping."""
    pid_bytes = proposal_id.to_bytes(64, "big")
    voter_bytes = encoding.decode_address(voter_addr)
    h = keccak.new(digest_bits=256)
    h.update(pid_bytes + voter_bytes)
    keccak_result = h.digest()
    return mapping_box_key("_hasVoted", keccak_result)


# --- Deploy ---

def test_deploy(voting):
    assert voting.app_id > 0


def test_admin(voting, voters):
    result = voting.send.call(au.AppClientMethodCallParams(method="admin"))
    assert result.abi_return == voters["admin"].address


def test_initial_proposal_count(voting):
    result = voting.send.call(au.AppClientMethodCallParams(method="proposalCount"))
    assert result.abi_return == 0


# --- Assign weights ---

def test_assign_weight_admin(voting, voters):
    app_id = voting.app_id
    admin_addr = addr_bytes(voters["admin"].address)
    wt_key = mapping_box_key("_weights", admin_addr)

    voting.send.call(
        au.AppClientMethodCallParams(
            method="assignWeight",
            args=[admin_addr, 100],
            box_references=[box_ref(app_id, wt_key)],
        )
    )


def test_assign_weight_v1(voting, voters):
    app_id = voting.app_id
    addr = addr_bytes(voters["v1"].address)
    wt_key = mapping_box_key("_weights", addr)

    voting.send.call(
        au.AppClientMethodCallParams(
            method="assignWeight",
            args=[addr, 50],
            box_references=[box_ref(app_id, wt_key)],
        )
    )


def test_assign_weight_v2(voting, voters):
    app_id = voting.app_id
    addr = addr_bytes(voters["v2"].address)
    wt_key = mapping_box_key("_weights", addr)

    voting.send.call(
        au.AppClientMethodCallParams(
            method="assignWeight",
            args=[addr, 30],
            box_references=[box_ref(app_id, wt_key)],
        )
    )


def test_total_weight(voting):
    result = voting.send.call(au.AppClientMethodCallParams(method="totalWeight"))
    assert result.abi_return == 180  # 100 + 50 + 30


def test_get_weight(voting, voters):
    app_id = voting.app_id
    addr = addr_bytes(voters["v1"].address)
    wt_key = mapping_box_key("_weights", addr)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getWeight",
            args=[addr],
            box_references=[box_ref(app_id, wt_key)],
        )
    )
    assert result.abi_return == 50


# --- Create proposal ---

def test_create_proposal(voting, voters):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    p_key = mapping_box_key("_proposers", pid)
    e_key = mapping_box_key("_endTimes", pid)
    s_key = mapping_box_key("_states", pid)
    q_key = mapping_box_key("_quorum", pid)
    admin_addr = addr_bytes(voters["admin"].address)
    wt_key = mapping_box_key("_weights", admin_addr)
    del_key = mapping_box_key("_delegates", admin_addr)

    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="createProposal",
            args=[1000, 100],  # duration=1000, quorum=100
            box_references=[
                box_ref(app_id, wt_key),
                box_ref(app_id, del_key),
                box_ref(app_id, p_key),
                box_ref(app_id, e_key),
                box_ref(app_id, s_key),
                box_ref(app_id, q_key),
            ],
        )
    )
    assert result.abi_return == 0  # first proposal


def test_proposal_count(voting):
    result = voting.send.call(au.AppClientMethodCallParams(method="proposalCount"))
    assert result.abi_return == 1


def test_proposal_state_active(voting):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    s_key = mapping_box_key("_states", pid)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getProposalState",
            args=[0],
            box_references=[box_ref(app_id, s_key)],
        )
    )
    assert result.abi_return == 1  # Active


# --- Vote ---

def test_admin_votes_for(voting, voters):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    s_key = mapping_box_key("_states", pid)
    admin_addr = addr_bytes(voters["admin"].address)
    wt_key = mapping_box_key("_weights", admin_addr)
    del_key = mapping_box_key("_delegates", admin_addr)
    vk = vote_box_key(0, voters["admin"].address)
    for_key = mapping_box_key("_forVotes", pid)
    against_key = mapping_box_key("_againstVotes", pid)

    voting.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, True],
            box_references=[
                box_ref(app_id, s_key),
                box_ref(app_id, wt_key),
                box_ref(app_id, del_key),
                box_ref(app_id, vk),
                box_ref(app_id, for_key),
                box_ref(app_id, against_key),
            ],
        )
    )


def test_has_voted(voting, voters):
    app_id = voting.app_id
    vk = vote_box_key(0, voters["admin"].address)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="hasVoted",
            args=[0, addr_bytes(voters["admin"].address)],
            box_references=[box_ref(app_id, vk)],
        )
    )
    assert result.abi_return is True


def test_v1_votes_for(voting, voters, localnet):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    s_key = mapping_box_key("_states", pid)
    v1_addr = addr_bytes(voters["v1"].address)
    wt_key = mapping_box_key("_weights", v1_addr)
    del_key = mapping_box_key("_delegates", v1_addr)
    vk = vote_box_key(0, voters["v1"].address)
    for_key = mapping_box_key("_forVotes", pid)
    against_key = mapping_box_key("_againstVotes", pid)

    v1_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=voting.app_spec,
            app_id=voting.app_id,
            default_sender=voters["v1"].address,
        )
    )
    v1_client.send.call(
        au.AppClientMethodCallParams(
            method="vote",
            args=[0, True],
            box_references=[
                box_ref(app_id, s_key),
                box_ref(app_id, wt_key),
                box_ref(app_id, del_key),
                box_ref(app_id, vk),
                box_ref(app_id, for_key),
                box_ref(app_id, against_key),
            ],
        )
    )


def test_proposal_votes(voting):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    for_key = mapping_box_key("_forVotes", pid)
    against_key = mapping_box_key("_againstVotes", pid)
    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getProposalVotes",
            args=[0],
            box_references=[
                box_ref(app_id, for_key),
                box_ref(app_id, against_key),
            ],
        )
    )
    ret = result.abi_return
    if isinstance(ret, dict):
        assert ret["forVotes"] == 150  # admin(100) + v1(50)
        assert ret["againstVotes"] == 0
    else:
        assert ret[0] == 150
        assert ret[1] == 0


# --- Cannot vote twice ---

def test_cannot_vote_twice(voting, voters):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    s_key = mapping_box_key("_states", pid)
    admin_addr = addr_bytes(voters["admin"].address)
    wt_key = mapping_box_key("_weights", admin_addr)
    del_key = mapping_box_key("_delegates", admin_addr)
    vk = vote_box_key(0, voters["admin"].address)
    for_key = mapping_box_key("_forVotes", pid)
    against_key = mapping_box_key("_againstVotes", pid)

    with pytest.raises(Exception):
        voting.send.call(
            au.AppClientMethodCallParams(
                method="vote",
                args=[0, True],
                box_references=[
                    box_ref(app_id, s_key),
                    box_ref(app_id, wt_key),
                    box_ref(app_id, del_key),
                    box_ref(app_id, vk),
                    box_ref(app_id, for_key),
                    box_ref(app_id, against_key),
                ],
            )
        )


# --- Early finalize (quorum met) ---

def test_early_finalize(voting, voters):
    app_id = voting.app_id
    pid = proposal_id_bytes(0)
    s_key = mapping_box_key("_states", pid)
    e_key = mapping_box_key("_endTimes", pid)
    for_key = mapping_box_key("_forVotes", pid)
    against_key = mapping_box_key("_againstVotes", pid)
    q_key = mapping_box_key("_quorum", pid)

    # Quorum is 100, we have 150 for-votes. Should pass.
    voting.send.call(
        au.AppClientMethodCallParams(
            method="finalize",
            args=[0],
            box_references=[
                box_ref(app_id, s_key),
                box_ref(app_id, e_key),
                box_ref(app_id, for_key),
                box_ref(app_id, against_key),
                box_ref(app_id, q_key),
            ],
        )
    )

    result = voting.send.call(
        au.AppClientMethodCallParams(
            method="getProposalState",
            args=[0],
            box_references=[box_ref(app_id, s_key)],
        )
    )
    assert result.abi_return == 2  # Passed


# --- Non-admin cannot assign weight ---

def test_non_admin_cannot_assign(voting, voters, localnet):
    app_id = voting.app_id
    addr = addr_bytes(voters["v1"].address)
    wt_key = mapping_box_key("_weights", addr)

    v1_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=voting.app_spec,
            app_id=voting.app_id,
            default_sender=voters["v1"].address,
        )
    )
    with pytest.raises(Exception):
        v1_client.send.call(
            au.AppClientMethodCallParams(
                method="assignWeight",
                args=[addr, 999],
                box_references=[box_ref(app_id, wt_key)],
            )
        )
