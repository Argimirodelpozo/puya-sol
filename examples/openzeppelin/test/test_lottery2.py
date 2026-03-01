"""
Lottery2 behavioral tests.
Tests round creation, ticket purchase, and winner drawing.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def price_key(rid):
    return mapping_box_key("_roundTicketPrice", rid.to_bytes(64, "big"))

def max_key(rid):
    return mapping_box_key("_roundMaxTickets", rid.to_bytes(64, "big"))

def sold_key(rid):
    return mapping_box_key("_roundTicketsSold", rid.to_bytes(64, "big"))

def drawn_key(rid):
    return mapping_box_key("_roundDrawn", rid.to_bytes(64, "big"))

def winner_key(rid):
    return mapping_box_key("_roundWinnerIndex", rid.to_bytes(64, "big"))

def prize_key(rid):
    return mapping_box_key("_roundPrize", rid.to_bytes(64, "big"))

def round_boxes(rid):
    return [
        au.BoxReference(app_id=0, name=price_key(rid)),
        au.BoxReference(app_id=0, name=max_key(rid)),
        au.BoxReference(app_id=0, name=sold_key(rid)),
        au.BoxReference(app_id=0, name=drawn_key(rid)),
        au.BoxReference(app_id=0, name=winner_key(rid)),
        au.BoxReference(app_id=0, name=prize_key(rid)),
    ]


@pytest.fixture(scope="module")
def lot(localnet, account):
    return deploy_contract(localnet, account, "Lottery2Test")


def test_deploy(lot):
    assert lot.app_id > 0

def test_admin(lot, account):
    r = lot.send.call(au.AppClientMethodCallParams(method="admin"))
    assert r.abi_return == account.address

def test_create_round(lot):
    boxes = round_boxes(0)
    r = lot.send.call(au.AppClientMethodCallParams(
        method="createRound", args=[100, 10], box_references=boxes))
    assert r.abi_return == 0

def test_round_count(lot):
    r = lot.send.call(au.AppClientMethodCallParams(method="roundCount"))
    assert r.abi_return == 1

def test_ticket_price(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="getRoundTicketPrice", args=[0],
        box_references=[au.BoxReference(app_id=0, name=price_key(0))]))
    assert r.abi_return == 100

def test_max_tickets(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="getRoundMaxTickets", args=[0],
        box_references=[au.BoxReference(app_id=0, name=max_key(0))]))
    assert r.abi_return == 10

def test_buy_ticket(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="buyTicket", args=[0],
        box_references=[
            au.BoxReference(app_id=0, name=drawn_key(0)),
            au.BoxReference(app_id=0, name=sold_key(0)),
            au.BoxReference(app_id=0, name=max_key(0)),
            au.BoxReference(app_id=0, name=price_key(0)),
            au.BoxReference(app_id=0, name=prize_key(0)),
        ]))
    assert r.abi_return == 0  # first ticket

def test_buy_more_tickets(lot):
    for i in range(4):
        lot.send.call(au.AppClientMethodCallParams(
            method="buyTicket", args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=drawn_key(0)),
                au.BoxReference(app_id=0, name=sold_key(0)),
                au.BoxReference(app_id=0, name=max_key(0)),
                au.BoxReference(app_id=0, name=price_key(0)),
                au.BoxReference(app_id=0, name=prize_key(0)),
            ], note=f"bt{i+2}".encode()))

def test_tickets_sold(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="getRoundTicketsSold", args=[0],
        box_references=[au.BoxReference(app_id=0, name=sold_key(0))]))
    assert r.abi_return == 5

def test_prize_pool(lot):
    # 5 tickets * 100 = 500
    r = lot.send.call(au.AppClientMethodCallParams(
        method="getRoundPrize", args=[0],
        box_references=[au.BoxReference(app_id=0, name=prize_key(0))]))
    assert r.abi_return == 500

def test_total_tickets_sold(lot):
    r = lot.send.call(au.AppClientMethodCallParams(method="totalTicketsSold"))
    assert r.abi_return == 5

def test_not_drawn(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="isRoundDrawn", args=[0],
        box_references=[au.BoxReference(app_id=0, name=drawn_key(0))]))
    assert r.abi_return is False

def test_draw_winner(lot):
    lot.send.call(au.AppClientMethodCallParams(
        method="drawWinner", args=[0, 2],  # winner is ticket index 2
        box_references=[
            au.BoxReference(app_id=0, name=drawn_key(0)),
            au.BoxReference(app_id=0, name=sold_key(0)),
            au.BoxReference(app_id=0, name=winner_key(0)),
            au.BoxReference(app_id=0, name=prize_key(0)),
        ]))

def test_drawn(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="isRoundDrawn", args=[0],
        box_references=[au.BoxReference(app_id=0, name=drawn_key(0))],
        note=b"d2"))
    assert r.abi_return is True

def test_winner_index(lot):
    r = lot.send.call(au.AppClientMethodCallParams(
        method="getRoundWinnerIndex", args=[0],
        box_references=[au.BoxReference(app_id=0, name=winner_key(0))]))
    assert r.abi_return == 2

def test_total_prizes_paid(lot):
    r = lot.send.call(au.AppClientMethodCallParams(method="totalPrizesPaid"))
    assert r.abi_return == 500
