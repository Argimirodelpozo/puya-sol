"""
Governance: Compound-inspired on-chain governance tests.
Tests: GovernorToken (ERC20 + delegation) and Governor (proposals + voting).
Exercises: cross-contract calls, flat mapping storage, nested mappings,
voting power delegation, proposal lifecycle.
"""

import hashlib

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, fund_contract, mapping_box_key, box_ref

INNER_FEE = au.AlgoAmount(micro_algo=2000)
INITIAL_SUPPLY = 10_000


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def pad64(value: int) -> bytes:
    """Pad integer to 64 bytes (uint512 ARC4 encoding)."""
    return value.to_bytes(64, "big")


# ─── GovernorToken Tests ───


@pytest.fixture(scope="module")
def token_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy GovernorToken then mint initial supply."""
    name_bytes = b"GovToken"
    symbol_bytes = b"GOV"

    client = deploy_contract(
        localnet, account, "GovernorToken",
        app_args=[name_bytes, symbol_bytes],
        fund_amount=2_000_000,
    )

    # Mint initial supply (constructor doesn't do box writes)
    app_id = client.app_id
    addr_b = addr_bytes(account.address)
    balance_box = mapping_box_key("_balances", addr_b)
    delegates_box = mapping_box_key("_delegates", addr_b)

    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, INITIAL_SUPPLY],
            box_references=[
                box_ref(app_id, balance_box),
                box_ref(app_id, delegates_box),
            ],
        )
    )

    return client


@pytest.mark.localnet
def test_token_balance(token_client: au.AppClient, account: SigningAccount) -> None:
    """Initial supply should be minted to deployer."""
    owner_box = mapping_box_key("_balances", addr_bytes(account.address))
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(token_client.app_id, owner_box)],
        )
    )
    assert result.abi_return == INITIAL_SUPPLY


@pytest.mark.localnet
def test_token_delegate_self(token_client: au.AppClient, account: SigningAccount) -> None:
    """Self-delegation should give voting power equal to balance."""
    app_id = token_client.app_id
    addr = account.address
    addr_b = addr_bytes(addr)

    delegate_box = mapping_box_key("_delegates", addr_b)
    power_box = mapping_box_key("_votingPower", addr_b)
    balance_box = mapping_box_key("_balances", addr_b)

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="delegate",
            args=[addr],
            box_references=[
                box_ref(app_id, delegate_box),
                box_ref(app_id, power_box),
                box_ref(app_id, balance_box),
            ],
        )
    )

    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="getVotingPower",
            args=[addr],
            box_references=[box_ref(app_id, power_box)],
        )
    )
    assert result.abi_return == INITIAL_SUPPLY


@pytest.mark.localnet
def test_token_delegates_returns_self(token_client: au.AppClient, account: SigningAccount) -> None:
    """After self-delegation, delegates() should return own address."""
    delegate_box = mapping_box_key("_delegates", addr_bytes(account.address))
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="delegates",
            args=[account.address],
            box_references=[box_ref(token_client.app_id, delegate_box)],
        )
    )
    assert result.abi_return == account.address


# ─── Governor Tests ───


@pytest.fixture(scope="module")
def governor_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    token_client: au.AppClient,
) -> au.AppClient:
    """Deploy Governor with reference to GovernorToken."""
    token_addr = token_client.app_id.to_bytes(32, "big")
    quorum = pad64(100)           # 100 votes for quorum
    threshold = pad64(10)         # 10 tokens to propose

    client = deploy_contract(
        localnet, account, "Governor",
        app_args=[token_addr, quorum, threshold],
        fund_amount=1_000_000,
    )
    return client


@pytest.mark.localnet
def test_propose(
    governor_client: au.AppClient,
    token_client: au.AppClient,
    account: SigningAccount,
) -> None:
    """Create a proposal — should return proposal ID 1."""
    app_id = governor_client.app_id
    token_app_id = token_client.app_id

    # Box refs for the voting power check (inner call to token)
    power_box = mapping_box_key("_votingPower", addr_bytes(account.address))

    # Proposal storage: _proposalProposer[1]
    proposer_box = mapping_box_key("_proposalProposer", pad64(1))

    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="propose",
            args=["Increase treasury allocation"],
            app_references=[token_app_id],
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(token_app_id, power_box),
            ],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == 1  # First proposal


@pytest.mark.localnet
def test_proposal_count(governor_client: au.AppClient) -> None:
    """Proposal count should be 1 after creating one proposal."""
    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="getProposalCount",
            args=[],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_cast_vote_for(
    governor_client: au.AppClient,
    token_client: au.AppClient,
    account: SigningAccount,
) -> None:
    """Vote FOR proposal 1."""
    app_id = governor_client.app_id
    token_app_id = token_client.app_id
    addr_b = addr_bytes(account.address)
    pid = pad64(1)

    # Receipt boxes (nested mapping: proposalId + voter)
    has_voted_box = mapping_box_key("_receiptHasVoted", pid, addr_b)
    support_box = mapping_box_key("_receiptSupport", pid, addr_b)
    votes_box = mapping_box_key("_receiptVotes", pid, addr_b)

    # Proposal vote count box
    for_votes_box = mapping_box_key("_proposalForVotes", pid)

    # Token voting power box
    power_box = mapping_box_key("_votingPower", addr_b)

    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="castVote",
            args=[1, 1],  # proposalId=1, support=1 (For)
            app_references=[token_app_id],
            box_references=[
                box_ref(app_id, has_voted_box),
                box_ref(app_id, support_box),
                box_ref(app_id, votes_box),
                box_ref(app_id, for_votes_box),
                box_ref(token_app_id, power_box),
            ],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == INITIAL_SUPPLY  # votes = voting power


@pytest.mark.localnet
def test_get_proposal_after_vote(
    governor_client: au.AppClient, account: SigningAccount
) -> None:
    """After voting, forVotes should equal the voter's power."""
    app_id = governor_client.app_id
    pid = pad64(1)

    proposer_box = mapping_box_key("_proposalProposer", pid)
    for_box = mapping_box_key("_proposalForVotes", pid)
    against_box = mapping_box_key("_proposalAgainstVotes", pid)
    abstain_box = mapping_box_key("_proposalAbstainVotes", pid)
    exec_box = mapping_box_key("_proposalExecuted", pid)
    cancel_box = mapping_box_key("_proposalCancelled", pid)

    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="getProposal",
            args=[1],
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(app_id, for_box),
                box_ref(app_id, against_box),
                box_ref(app_id, abstain_box),
                box_ref(app_id, exec_box),
                box_ref(app_id, cancel_box),
            ],
        )
    )
    # Returns (proposer, forVotes, againstVotes, abstainVotes, executed, cancelled)
    proposer, for_votes, against_votes, abstain_votes, executed, cancelled = result.abi_return
    assert for_votes == INITIAL_SUPPLY
    assert against_votes == 0
    assert abstain_votes == 0
    assert executed is False
    assert cancelled is False


@pytest.mark.localnet
def test_execute_proposal(governor_client: au.AppClient) -> None:
    """Execute proposal 1 — should succeed since forVotes > quorum."""
    app_id = governor_client.app_id
    pid = pad64(1)

    exec_box = mapping_box_key("_proposalExecuted", pid)
    cancel_box = mapping_box_key("_proposalCancelled", pid)
    for_box = mapping_box_key("_proposalForVotes", pid)
    against_box = mapping_box_key("_proposalAgainstVotes", pid)
    abstain_box = mapping_box_key("_proposalAbstainVotes", pid)

    governor_client.send.call(
        au.AppClientMethodCallParams(
            method="execute",
            args=[1],
            box_references=[
                box_ref(app_id, exec_box),
                box_ref(app_id, cancel_box),
                box_ref(app_id, for_box),
                box_ref(app_id, against_box),
                box_ref(app_id, abstain_box),
            ],
        )
    )

    # Verify executed flag
    proposer_box = mapping_box_key("_proposalProposer", pid)
    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="getProposal",
            args=[1],
            box_references=[
                box_ref(app_id, proposer_box),
                box_ref(app_id, for_box),
                box_ref(app_id, against_box),
                box_ref(app_id, abstain_box),
                box_ref(app_id, exec_box),
                box_ref(app_id, cancel_box),
            ],
        )
    )
    _, _, _, _, executed, _ = result.abi_return
    assert executed is True


@pytest.mark.localnet
def test_get_receipt(
    governor_client: au.AppClient,
    account: SigningAccount,
) -> None:
    """Verify vote receipt for the voter."""
    app_id = governor_client.app_id
    pid = pad64(1)
    addr_b = addr_bytes(account.address)

    has_voted_box = mapping_box_key("_receiptHasVoted", pid, addr_b)
    support_box = mapping_box_key("_receiptSupport", pid, addr_b)
    votes_box = mapping_box_key("_receiptVotes", pid, addr_b)

    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="getReceipt",
            args=[1, account.address],
            box_references=[
                box_ref(app_id, has_voted_box),
                box_ref(app_id, support_box),
                box_ref(app_id, votes_box),
            ],
        )
    )
    has_voted, support, votes = result.abi_return
    assert has_voted is True
    assert support == 1  # For
    assert votes == INITIAL_SUPPLY


@pytest.mark.localnet
def test_propose_second_and_cancel(
    governor_client: au.AppClient,
    token_client: au.AppClient,
    account: SigningAccount,
) -> None:
    """Create proposal 2 then cancel it."""
    app_id = governor_client.app_id
    token_app_id = token_client.app_id
    pid2 = pad64(2)

    power_box = mapping_box_key("_votingPower", addr_bytes(account.address))
    proposer_box_2 = mapping_box_key("_proposalProposer", pid2)

    # Create proposal 2
    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="propose",
            args=["Reduce fees"],
            app_references=[token_app_id],
            box_references=[
                box_ref(app_id, proposer_box_2),
                box_ref(token_app_id, power_box),
            ],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == 2

    # Cancel proposal 2
    exec_box_2 = mapping_box_key("_proposalExecuted", pid2)
    cancel_box_2 = mapping_box_key("_proposalCancelled", pid2)

    governor_client.send.call(
        au.AppClientMethodCallParams(
            method="cancel",
            args=[2],
            box_references=[
                box_ref(app_id, exec_box_2),
                box_ref(app_id, cancel_box_2),
                box_ref(app_id, proposer_box_2),
            ],
        )
    )

    # Verify cancelled
    for_box_2 = mapping_box_key("_proposalForVotes", pid2)
    against_box_2 = mapping_box_key("_proposalAgainstVotes", pid2)
    abstain_box_2 = mapping_box_key("_proposalAbstainVotes", pid2)

    result = governor_client.send.call(
        au.AppClientMethodCallParams(
            method="getProposal",
            args=[2],
            box_references=[
                box_ref(app_id, proposer_box_2),
                box_ref(app_id, for_box_2),
                box_ref(app_id, against_box_2),
                box_ref(app_id, abstain_box_2),
                box_ref(app_id, exec_box_2),
                box_ref(app_id, cancel_box_2),
            ],
        )
    )
    _, _, _, _, executed, cancelled = result.abi_return
    assert executed is False
    assert cancelled is True
