"""Regression tests for inheritance: virtual/override, abstract, multiple contracts."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Inheritance.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def child(compiled, localnet, account):
    c = compiled["Child"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


@pytest.fixture(scope="module")
def concrete(compiled, localnet, account):
    c = compiled["ConcreteChild"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestInheritance:
    def test_inherited_method(self, child):
        """Child inherits getBase from Base."""
        r = child.send.call(au.AppClientMethodCallParams(method="getBase"))
        assert r.abi_return == 0

    def test_override(self, child):
        """Child.setBase overrides Base.setBase (doubles value)."""
        child.send.call(au.AppClientMethodCallParams(method="setBase", args=[10]))
        assert child.send.call(au.AppClientMethodCallParams(method="getBase")).abi_return == 20

    def test_child_own_method(self, child):
        child.send.call(au.AppClientMethodCallParams(method="setChild", args=[99]))
        assert child.send.call(au.AppClientMethodCallParams(method="childValue")).abi_return == 99


class TestAbstract:
    def test_concrete_implements_abstract(self, concrete):
        r = concrete.send.call(au.AppClientMethodCallParams(method="abstractMethod"))
        assert r.abi_return == 42
