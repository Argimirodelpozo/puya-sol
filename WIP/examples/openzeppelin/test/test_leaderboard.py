"""
Leaderboard behavioral tests.
Tests player registration, score updates, and high score tracking.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def hash_key(pid):
    return mapping_box_key("_playerHash", pid.to_bytes(64, "big"))
def score_key(pid):
    return mapping_box_key("_playerScore", pid.to_bytes(64, "big"))

def player_boxes(pid):
    return [
        au.BoxReference(app_id=0, name=hash_key(pid)),
        au.BoxReference(app_id=0, name=score_key(pid)),
    ]

@pytest.fixture(scope="module")
def lb(localnet, account):
    return deploy_contract(localnet, account, "LeaderboardTest")

def test_deploy(lb):
    assert lb.app_id > 0

def test_admin(lb, account):
    r = lb.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_register_player(lb):
    boxes = player_boxes(0)
    lb.send.call(au.AppClientMethodCallParams(
        method="initPlayer", args=[0], box_references=boxes))
    r = lb.send.call(au.AppClientMethodCallParams(
        method="registerPlayer", args=[111],
        box_references=boxes))
    assert r.abi_return == 0

def test_player_count(lb):
    r = lb.send.call(au.AppClientMethodCallParams(method="getPlayerCount"))
    assert r.abi_return == 1

def test_player_hash(lb):
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getPlayerHash", args=[0],
        box_references=[au.BoxReference(app_id=0, name=hash_key(0))]))
    assert r.abi_return == 111

def test_initial_score(lb):
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getPlayerScore", args=[0],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))]))
    assert r.abi_return == 0

def test_update_score(lb):
    lb.send.call(au.AppClientMethodCallParams(
        method="updateScore", args=[0, 500],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))]))

def test_score_after_update(lb):
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getPlayerScore", args=[0],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))],
        note=b"s2"))
    assert r.abi_return == 500

def test_high_score(lb):
    r = lb.send.call(au.AppClientMethodCallParams(method="getHighScore"))
    assert r.abi_return == 500

def test_total_games(lb):
    r = lb.send.call(au.AppClientMethodCallParams(method="getTotalGamesPlayed"))
    assert r.abi_return == 1

def test_register_second_player(lb):
    boxes = player_boxes(1)
    lb.send.call(au.AppClientMethodCallParams(
        method="initPlayer", args=[1], box_references=boxes))
    lb.send.call(au.AppClientMethodCallParams(
        method="registerPlayer", args=[222],
        box_references=boxes, note=b"r2"))
    lb.send.call(au.AppClientMethodCallParams(
        method="updateScore", args=[1, 800],
        box_references=[au.BoxReference(app_id=0, name=score_key(1))],
        note=b"u2"))

def test_new_high_score(lb):
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getHighScore", note=b"h2"))
    assert r.abi_return == 800

def test_total_games_after(lb):
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getTotalGamesPlayed", note=b"g2"))
    assert r.abi_return == 2

def test_lower_score_no_high(lb):
    lb.send.call(au.AppClientMethodCallParams(
        method="updateScore", args=[0, 300],
        box_references=[au.BoxReference(app_id=0, name=score_key(0))],
        note=b"u3"))
    r = lb.send.call(au.AppClientMethodCallParams(
        method="getHighScore", note=b"h3"))
    assert r.abi_return == 800  # unchanged
