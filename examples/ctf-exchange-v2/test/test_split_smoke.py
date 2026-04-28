"""Smoke test for v2 split deployment.

Verifies the orchestrator + helpers come up cleanly and __postInit ran. No
business logic yet — those land in test_ctfexchange.py and friends.
"""
import algokit_utils as au

from conftest import AUTO_POPULATE, addr


def test_split_deploys(split_exchange):
    h1, h2, orch = split_exchange
    assert h1.app_id > 0
    assert h2.app_id > 0
    assert orch.app_id > 0
    assert len({h1.app_id, h2.app_id, orch.app_id}) == 3


def test_split_admin_is_deployer(split_exchange, admin):
    """ExchangeInitParams.admin = admin's address; admin should have admin
    role after __postInit."""
    _, _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="isAdmin", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return is True


def test_split_admin_is_operator(split_exchange, admin):
    _, _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="isOperator", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return is True
