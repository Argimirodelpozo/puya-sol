"""Regression tests for Solidity structs: memory, storage, mapping of structs."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, mapping_box_key, box_ref

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Structs.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def app(compiled, localnet, account):
    c = compiled["Structs"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestMemoryStructs:
    def test_add_points(self, app):
        r = app.send.call(au.AppClientMethodCallParams(
            method="addPoints", args=[1, 2, 3, 4],
        ))
        assert r.abi_return == [4, 6] or r.abi_return == (4, 6)


class TestStorageStruct:
    def test_set_and_get_origin(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setOrigin", args=[10, 20]))
        r = app.send.call(au.AppClientMethodCallParams(method="getOrigin"))
        ret = r.abi_return
        if isinstance(ret, dict):
            assert ret.get("x", ret.get(0)) == 10
        else:
            assert ret[0] == 10 and ret[1] == 20


class TestMappingOfStructs:
    def test_set_and_get_point(self, app):
        bx = box_ref(app.app_id, mapping_box_key("points", (1).to_bytes(32, "big")))
        app.send.call(au.AppClientMethodCallParams(
            method="setPoint", args=[1, 100, 200],
            box_references=[bx],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getPointX", args=[1],
            box_references=[bx],
        ))
        assert r.abi_return == 100
