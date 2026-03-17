"""Regression tests for string operations."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "StringOps.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def app(compiled, localnet, account):
    c = compiled["StringOps"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestStringStorage:
    def test_set_and_get(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setString", args=["hello"]))
        r = app.send.call(au.AppClientMethodCallParams(method="getString"))
        assert r.abi_return == "hello"

    def test_return_literal(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="returnLiteral")).abi_return == "hello world"


class TestStringOperations:
    def test_length(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="stringLength", args=["abc"])).abi_return == 3
        assert app.send.call(au.AppClientMethodCallParams(method="stringLength", args=[""])).abi_return == 0

    def test_compare_equal(self, app):
        assert app.send.call(au.AppClientMethodCallParams(
            method="compareStrings", args=["hello", "hello"]
        )).abi_return is True

    def test_compare_different(self, app):
        assert app.send.call(au.AppClientMethodCallParams(
            method="compareStrings", args=["hello", "world"]
        )).abi_return is False

    def test_concat(self, app):
        r = app.send.call(au.AppClientMethodCallParams(
            method="concatStrings", args=["hello ", "world"]
        ))
        assert r.abi_return == "hello world"
