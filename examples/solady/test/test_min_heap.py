"""Tests for Solady MinHeapLib compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/MinHeapLib.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "MinHeapWrapper")
    return make_caller(algod_client, account, app_id, spec)


@pytest.mark.xfail(reason="box storage requires pre-created boxes for heap data")
class TestMinHeap:
    def test_initial_length(self, call):
        assert call("length") == 0

    def test_push_single(self, call):
        call("push", 5)
        assert call("length") == 1
        assert call("root") == 5

    def test_push_smaller(self, call):
        call("push", 3)
        assert call("length") == 2
        assert call("root") == 3

    def test_push_larger(self, call):
        call("push", 7)
        assert call("length") == 3
        assert call("root") == 3

    def test_pop_returns_min(self, call):
        result = call("pop")
        assert result == 3
        assert call("length") == 2

    def test_pop_next_min(self, call):
        result = call("pop")
        assert result == 5
        assert call("length") == 1

    def test_pop_last(self, call):
        result = call("pop")
        assert result == 7
        assert call("length") == 0
