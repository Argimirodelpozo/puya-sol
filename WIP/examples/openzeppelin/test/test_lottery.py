"""
Lottery behavioral tests.
Tests entering, picking winner, rounds, and toggling.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def player_key(idx):
    return mapping_box_key("_players", idx.to_bytes(64, "big"))


@pytest.fixture(scope="module")
def lottery(localnet, account):
    return deploy_contract(localnet, account, "LotteryTest")


def test_deploy(lottery):
    assert lottery.app_id > 0


def test_manager(lottery, account):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="manager")
    )
    assert result.abi_return == account.address


def test_initial_price(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="ticketPrice")
    )
    assert result.abi_return == 100


def test_initial_active(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="isActive")
    )
    assert result.abi_return is True


def test_initial_round(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="roundNumber")
    )
    assert result.abi_return == 1


def test_enter(lottery, account):
    lottery.send.call(
        au.AppClientMethodCallParams(
            method="enter",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=player_key(1))],
        )
    )


def test_player_count(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="playerCount")
    )
    assert result.abi_return == 1


def test_prize_pool(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="prizePool")
    )
    assert result.abi_return == 100


def test_get_player(lottery, account):
    result = lottery.send.call(
        au.AppClientMethodCallParams(
            method="getPlayer",
            args=[1],
            box_references=[au.BoxReference(app_id=0, name=player_key(1))],
        )
    )
    assert result.abi_return == account.address


def test_enter_more(lottery, account):
    """Enter 2 more players (same address for testing)."""
    for i in range(2):
        lottery.send.call(
            au.AppClientMethodCallParams(
                method="enter",
                args=[account.address],
                box_references=[au.BoxReference(app_id=0, name=player_key(i + 2))],
                note=f"enter_{i+2}".encode(),
            )
        )


def test_player_count_after(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="playerCount", note=b"pc2")
    )
    assert result.abi_return == 3


def test_pick_winner(lottery, account):
    # seed=7, 7 % 3 = 1, winner index = 2
    boxes = [
        au.BoxReference(app_id=0, name=player_key(1)),
        au.BoxReference(app_id=0, name=player_key(2)),
        au.BoxReference(app_id=0, name=player_key(3)),
    ]
    result = lottery.send.call(
        au.AppClientMethodCallParams(
            method="pickWinner",
            args=[7],
            box_references=boxes,
        )
    )
    assert result.abi_return == account.address


def test_last_winner(lottery, account):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="lastWinner")
    )
    assert result.abi_return == account.address


def test_round_incremented(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="roundNumber", note=b"rn2")
    )
    assert result.abi_return == 2


def test_pool_reset(lottery):
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="prizePool", note=b"pp2")
    )
    assert result.abi_return == 0


def test_toggle_active(lottery):
    lottery.send.call(
        au.AppClientMethodCallParams(method="toggleActive")
    )
    result = lottery.send.call(
        au.AppClientMethodCallParams(method="isActive", note=b"ia2")
    )
    assert result.abi_return is False
