"""Tests for Solady SafeCastLib compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/SafeCastLib.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "SafeCastWrapper", extra_pages=1)
    return make_caller(algod_client, account, app_id, spec)


class TestToUint8:
    def test_zero(self, call):
        assert call("toUint8", 0) == 0

    def test_max_valid(self, call):
        assert call("toUint8", 255) == 255

    def test_one(self, call):
        assert call("toUint8", 1) == 1

    def test_mid(self, call):
        assert call("toUint8", 128) == 128


class TestToUint16:
    def test_zero(self, call):
        assert call("toUint16", 0) == 0

    def test_max_valid(self, call):
        assert call("toUint16", 65535) == 65535

    def test_one(self, call):
        assert call("toUint16", 1) == 1


class TestToUint32:
    def test_zero(self, call):
        assert call("toUint32", 0) == 0

    def test_max_valid(self, call):
        assert call("toUint32", 2**32 - 1) == 2**32 - 1

    def test_one(self, call):
        assert call("toUint32", 1) == 1


class TestToUint64:
    def test_zero(self, call):
        assert call("toUint64", 0) == 0

    def test_max_valid(self, call):
        assert call("toUint64", 2**64 - 1) == 2**64 - 1

    def test_one(self, call):
        assert call("toUint64", 1) == 1


class TestToUint128:
    def test_zero(self, call):
        assert call("toUint128", 0) == 0

    def test_max_valid(self, call):
        assert call("toUint128", 2**128 - 1) == 2**128 - 1

    def test_one(self, call):
        assert call("toUint128", 1) == 1


class TestToInt8:
    def test_zero(self, call):
        assert call("toInt8", 0) == 0

    def test_max_valid(self, call):
        # int8 max = 127
        assert call("toInt8", 127) == 127

    def test_one(self, call):
        assert call("toInt8", 1) == 1


class TestToInt256:
    def test_zero(self, call):
        assert call("toInt256", 0) == 0

    def test_max_valid(self, call):
        # int256 max = 2^255 - 1
        assert call("toInt256", 2**255 - 1) == 2**255 - 1

    def test_one(self, call):
        assert call("toInt256", 1) == 1


class TestToUint256:
    def test_zero(self, call):
        assert call("toUint256", 0) == 0

    def test_one(self, call):
        assert call("toUint256", 1) == 1

    def test_large(self, call):
        assert call("toUint256", 2**200) == 2**200
