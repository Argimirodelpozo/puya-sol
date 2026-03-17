"""Regression tests for multiple contracts in a single .sol file."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, mapping_box_key, box_ref

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "MultiContract.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


class TestCompilation:
    def test_all_three_compiled(self, compiled):
        """Each contract should produce separate artifacts."""
        assert "Counter" in compiled
        assert "Storage" in compiled
        assert "Calculator" in compiled

    def test_each_has_binary(self, compiled):
        for name in ("Counter", "Storage", "Calculator"):
            assert compiled[name]["approval_bin"].exists()


class TestCounter:
    @pytest.fixture
    def counter(self, compiled, localnet, account):
        c = compiled["Counter"]
        return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])

    def test_initial_zero(self, counter):
        assert counter.send.call(au.AppClientMethodCallParams(method="getCount")).abi_return == 0

    def test_increment(self, counter):
        counter.send.call(au.AppClientMethodCallParams(method="increment"))
        counter.send.call(au.AppClientMethodCallParams(method="increment"))
        assert counter.send.call(au.AppClientMethodCallParams(method="getCount")).abi_return == 2


class TestStorage:
    @pytest.fixture
    def storage(self, compiled, localnet, account):
        c = compiled["Storage"]
        return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])

    def test_store_and_load(self, storage):
        key = [0xAB] * 32
        bx = box_ref(storage.app_id, mapping_box_key("data", bytes(key)))
        storage.send.call(au.AppClientMethodCallParams(
            method="store", args=[key, 42],
            box_references=[bx],
        ))
        r = storage.send.call(au.AppClientMethodCallParams(
            method="load", args=[key],
            box_references=[bx],
        ))
        assert r.abi_return == 42


class TestCalculator:
    @pytest.fixture(scope="class")
    def calc(self, compiled, localnet, account):
        c = compiled["Calculator"]
        return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])

    def test_add(self, calc):
        assert calc.send.call(au.AppClientMethodCallParams(method="add", args=[3, 7])).abi_return == 10

    def test_multiply(self, calc):
        assert calc.send.call(au.AppClientMethodCallParams(method="multiply", args=[6, 7])).abi_return == 42

    def test_power(self, calc):
        assert calc.send.call(au.AppClientMethodCallParams(method="power", args=[2, 10])).abi_return == 1024


class TestIndependentDeployment:
    def test_separate_app_ids(self, compiled, localnet, account):
        """Each contract deploys as a separate app with unique ID."""
        apps = []
        for name in ("Counter", "Storage", "Calculator"):
            c = compiled[name]
            app = deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])
            apps.append(app.app_id)
        # All IDs should be unique
        assert len(set(apps)) == 3
