"""Tests for Solady EnumerableSetLib compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/EnumerableSetLib.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "EnumerableSetWrapper")
    return make_caller(algod_client, account, app_id, spec)


class TestEnumerableSet:
    def test_initial_length(self, call):
        assert call("length") == 0

    def test_add(self, call):
        result = call("add", 42)
        assert result is True

    def test_not_contains(self, call):
        assert call("contains", 99) is False

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_length_after_add(self, call):
        assert call("length") == 1

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_contains(self, call):
        assert call("contains", 42) is True

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_at_zero(self, call):
        assert call("at", 0) == 42

    def test_add_second(self, call):
        result = call("add", 100)
        assert result is True

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_add_duplicate(self, call):
        result = call("add", 42)
        assert result is False

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_remove(self, call):
        result = call("remove", 42)
        assert result is True

    def test_remove_nonexistent(self, call):
        result = call("remove", 42)
        assert result is False

    @pytest.mark.xfail(reason="box storage requires pre-created boxes")
    def test_remaining_element(self, call):
        assert call("contains", 100) is True
