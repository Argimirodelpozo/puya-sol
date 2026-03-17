"""Regression tests for function overloading (same name, different params)."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Overloading.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Overloading"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestParameterCountOverloading:
    def test_one_param(self, app):
        """compute(uint256) → a * 2"""
        r = app.send.call(au.AppClientMethodCallParams(
            method="compute(uint256)uint256", args=[5]
        ))
        assert r.abi_return == 10

    def test_two_params(self, app):
        """compute(uint256, uint256) → a + b"""
        r = app.send.call(au.AppClientMethodCallParams(
            method="compute(uint256,uint256)uint256", args=[3, 7]
        ))
        assert r.abi_return == 10

    def test_three_params(self, app):
        """compute(uint256, uint256, uint256) → a + b + c"""
        r = app.send.call(au.AppClientMethodCallParams(
            method="compute(uint256,uint256,uint256)uint256", args=[1, 2, 3]
        ))
        assert r.abi_return == 6


class TestParameterTypeOverloading:
    def test_convert_bool(self, app):
        """convert(bool) → uint256"""
        r = app.send.call(au.AppClientMethodCallParams(
            method="convert(bool)uint256", args=[True]
        ))
        assert r.abi_return == 1

    def test_convert_uint(self, app):
        """convert(uint256) → bool"""
        r = app.send.call(au.AppClientMethodCallParams(
            method="convert(uint256)bool", args=[42]
        ))
        assert r.abi_return is True

    def test_convert_uint_zero(self, app):
        r = app.send.call(au.AppClientMethodCallParams(
            method="convert(uint256)bool", args=[0]
        ))
        assert r.abi_return is False


class TestAddressVsUintOverloading:
    def test_identify_address(self, app, account):
        r = app.send.call(au.AppClientMethodCallParams(
            method="identify(address)uint256", args=[account.address]
        ))
        assert r.abi_return == 1

    def test_identify_uint(self, app):
        r = app.send.call(au.AppClientMethodCallParams(
            method="identify(uint256)uint256", args=[99]
        ))
        assert r.abi_return == 2


class TestInternalOverloading:
    def test_internal_one_param(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="useInternal1", args=[5])).abi_return == 10

    def test_internal_two_params(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="useInternal2", args=[3, 7])).abi_return == 20
