"""
ERC20Votes behavioral tests.
Tests voting power delegation, minting with delegation, and transfer with delegation.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def bal_key(addr):
    return mapping_box_key("_balances", encoding.decode_address(addr))


def del_key(addr):
    return mapping_box_key("_delegates", encoding.decode_address(addr))


def vp_key(addr):
    return mapping_box_key("_votingPower", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def votes(localnet, account):
    return deploy_contract(localnet, account, "ERC20VotesTest")


def test_deploy(votes):
    assert votes.app_id > 0


def test_name(votes):
    result = votes.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "VoteToken"


def test_symbol(votes):
    result = votes.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "VOTE"


def test_initial_supply(votes):
    result = votes.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == 0


def test_delegate_to_self(votes, account):
    """Delegate to self before minting."""
    boxes = [
        au.BoxReference(app_id=0, name=del_key(account.address)),
        au.BoxReference(app_id=0, name=vp_key(account.address)),
        au.BoxReference(app_id=0, name=bal_key(account.address)),
    ]
    votes.send.call(
        au.AppClientMethodCallParams(
            method="delegate",
            args=[account.address, account.address],
            box_references=boxes,
        )
    )


def test_delegates_returns_self(votes, account):
    result = votes.send.call(
        au.AppClientMethodCallParams(
            method="delegates",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=del_key(account.address))],
        )
    )
    assert result.abi_return == account.address


def test_mint_with_delegation(votes, account):
    """Mint 1000 tokens to account (already delegated to self)."""
    boxes = [
        au.BoxReference(app_id=0, name=bal_key(account.address)),
        au.BoxReference(app_id=0, name=del_key(account.address)),
        au.BoxReference(app_id=0, name=vp_key(account.address)),
    ]
    votes.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1000],
            box_references=boxes,
        )
    )


def test_balance_after_mint(votes, account):
    result = votes.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=bal_key(account.address))],
        )
    )
    assert result.abi_return == 1000


def test_voting_power_after_mint(votes, account):
    """Self-delegated, so voting power should equal balance."""
    result = votes.send.call(
        au.AppClientMethodCallParams(
            method="getVotes",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=vp_key(account.address))],
        )
    )
    assert result.abi_return == 1000


def test_total_supply_after_mint(votes):
    result = votes.send.call(
        au.AppClientMethodCallParams(method="totalSupply", note=b"ts1")
    )
    assert result.abi_return == 1000


def test_mint_more(votes, account):
    boxes = [
        au.BoxReference(app_id=0, name=bal_key(account.address)),
        au.BoxReference(app_id=0, name=del_key(account.address)),
        au.BoxReference(app_id=0, name=vp_key(account.address)),
    ]
    votes.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 500],
            box_references=boxes,
        )
    )


def test_voting_power_after_second_mint(votes, account):
    result = votes.send.call(
        au.AppClientMethodCallParams(
            method="getVotes",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=vp_key(account.address))],
            note=b"vp2",
        )
    )
    assert result.abi_return == 1500
