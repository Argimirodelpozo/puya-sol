"""Frontend-only checks for the declaration facts used by call boundaries."""

import json
import subprocess

import pytest

from .compile import _puya_sol_cmd
from .paths import TESTS_DIR


@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_reference_transfer_call_boundaries(tmp_path, slot):
    source = TESTS_DIR / "puyasolRegression/contracts/context_reference_transfers.sol"
    command = _puya_sol_cmd(source, [], tmp_path, None, [], None, False, None, None,
                            ["--evm-storage-layout"] if slot else [])
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=90)
    roots = json.loads((tmp_path / "awst.json").read_text())
    contracts = {root["name"]: root for root in roots if root["_type"] == "Contract"}
    methods = contracts["ContextOffsetDerived"]["methods"]
    references = [method for method in methods if method["args"] and method["args"][0]["name"] == "item"]
    assert references
    for method in references:
        assert [arg["name"] for arg in method["args"]] == (["item"] if slot else ["item", "item__off"])
    mutations = [method for method in contracts["ContextMemoryTransfers"]["methods"]
                 if [arg["name"] for arg in method["args"]] == ["p"]]
    assert len(mutations) == 3
    assert all(method["return_type"]["name"] != "void" for method in mutations)
