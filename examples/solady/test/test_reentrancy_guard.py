"""Tests for Solady ReentrancyGuard compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/ReentrancyGuard.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "ReentrancyGuardWrapper")
    return make_caller(algod_client, account, app_id, spec)


class TestReentrancyGuard:
    def test_initial_counter(self, call):
        assert call("getCounter") == 0

    def test_counter_alias(self, call):
        assert call("counter") == 0

    def test_increment(self, call):
        call("increment")
        assert call("getCounter") == 1

    def test_increment_twice(self, call):
        call("increment")
        assert call("getCounter") == 2

    def test_increment_three(self, call):
        call("increment")
        assert call("getCounter") == 3
