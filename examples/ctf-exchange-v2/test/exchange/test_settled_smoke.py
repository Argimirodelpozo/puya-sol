"""Smoke test for split_exchange_settled fixture."""

def test_settled_setup(split_exchange_settled):
    h1, h2, orch, usdc, ctf = split_exchange_settled
    assert orch.app_id > 0
