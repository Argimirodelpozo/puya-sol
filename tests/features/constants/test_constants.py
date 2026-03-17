"""Regression tests for constants: contract-level, file-level, immutable, type().max, constant expressions."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Constants.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Constants"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestFileLevelConstants:
    def test_file_level_const(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="getFileLevelConst")).abi_return == 12345

    def test_computed_const(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="getComputedConst")).abi_return == 2**128


class TestContractConstants:
    def test_precision_arithmetic(self, app):
        # 500 * 1e18 / 100 = 5e18
        r = app.send.call(au.AppClientMethodCallParams(method="withPrecision", args=[500]))
        assert r.abi_return == 500 * 10**18 // 100

    def test_max_supply_check_pass(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="checkMax", args=[1000]))
        assert r.abi_return is True

    def test_max_supply_check_fail(self, app):
        too_much = 1000001 * 10**18
        with pytest.raises(Exception, match="exceeds max|assert"):
            app.send.call(
                au.AppClientMethodCallParams(method="checkMax", args=[too_much]),
                send_params=au.SendParams(populate_app_call_resources=False),
            )

    def test_to_wei(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="toWei", args=[5])).abi_return == 5 * 10**18


class TestImmutables:
    def test_deploy_time_nonzero(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="getDeployTime"))
        assert r.abi_return > 0

    def test_deployer_is_sender(self, app, account):
        r = app.send.call(au.AppClientMethodCallParams(method="getDeployer"))
        assert r.abi_return == account.address


class TestTypeMax:
    def test_max_uint256(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="getMaxUint256")).abi_return == 2**256 - 1

    def test_max_uint64(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="getMaxUint64")).abi_return == 2**64 - 1


class TestConstantBytes:
    def test_empty_hash_true(self, app):
        zero = [0] * 32
        assert app.send.call(au.AppClientMethodCallParams(method="isEmptyHash", args=[zero])).abi_return is True

    def test_empty_hash_false(self, app):
        nonzero = [0] * 31 + [1]
        assert app.send.call(au.AppClientMethodCallParams(method="isEmptyHash", args=[nonzero])).abi_return is False
