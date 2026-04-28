"""Translation of v2 src/test/Preapproved.t.sol.

`preapproveOrder` is operator-gated; `invalidatePreapprovedOrder` is
maker-gated. The dispatch logic doesn't go through matchOrders, so these
SHOULD be testable — but they all need a signed Order to compute the hash
that gets stored. The signing pipeline isn't wired up yet, so each test
xfails for now.
"""
import pytest

pytestmark = pytest.mark.xfail(
    reason="Preapproved tests need eth-style EOA signing; pending",
    strict=False,
)


def test_preapprove_order_revert_invalid_signature(split_exchange):
    """test_preapproveOrder_revert_invalidSignature"""
    pytest.fail("EOA signing not wired up")


def test_preapprove_order_revert_not_operator(split_exchange):
    """test_preapproveOrder_revert_NotOperator"""
    pytest.fail("EOA signing not wired up")


def test_match_orders_invalidated_preapproval_reverts(split_exchange):
    """test_matchOrders_invalidatedPreapproval_reverts"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_maker_complementary(split_exchange):
    """test_matchOrders_preapprovedMaker_complementary"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_taker_complementary(split_exchange):
    """test_matchOrders_preapprovedTaker_complementary"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_respects_filled_status(split_exchange):
    """test_matchOrders_preapproved_respectsFilledStatus"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_respects_user_pause(split_exchange):
    """test_matchOrders_preapproved_respectsUserPause"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_partial_fill(split_exchange):
    """test_matchOrders_preapproved_partialFill"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_invalid_signature_not_preapproved_reverts(split_exchange):
    """test_matchOrders_invalidSignature_notPreapproved_reverts"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_empty_signature_preapproved_complementary(split_exchange):
    """test_matchOrders_emptySignature_preapproved_complementary"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_empty_signature_not_preapproved_reverts(split_exchange):
    """test_matchOrders_emptySignature_notPreapproved_reverts"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_empty_signature_invalidated_preapproval_reverts(split_exchange):
    """test_matchOrders_emptySignature_invalidatedPreapproval_reverts"""
    pytest.fail("matchOrders runtime not wired")


def test_match_orders_preapproved_1271_signer_invalidated(split_exchange):
    """test_matchOrders_preapproved1271_signerInvalidated"""
    pytest.fail("matchOrders runtime not wired")
