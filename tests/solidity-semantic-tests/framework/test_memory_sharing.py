"""The scratch sharing analysis must not turn all signatures into pointers."""
import json
import subprocess

from .compile import _puya_sol_cmd
from .paths import TESTS_DIR


def test_unique_memory_stays_value_backed(tmp_path):
    source = TESTS_DIR / 'puyasolRegression/contracts/sharing_unique.sol'
    command = _puya_sol_cmd(source, [], tmp_path, None, [], None, False, None, None,
                            ['--memory-model', 'scratch'])
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=90)
    roots = json.loads((tmp_path / 'awst.json').read_text())
    contract = next(root for root in roots if root.get('name') == 'SharingUnique')
    helpers = [method for method in contract['methods']
               if [argument['name'] for argument in method['args']] == ['items']]
    assert helpers
    assert all(method['args'][0]['wtype']['name'] != 'uint64' for method in helpers)
