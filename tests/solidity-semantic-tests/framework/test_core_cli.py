"""CLI cancellation, legacy import identity and backend completion regressions."""

import ctypes
import json
import os
import select
import signal
import subprocess
import sys
import time

import pytest

from .paths import COMPILER, PUYA


def command(tmp_path, backend=None):
    source = tmp_path / "C.sol"
    source.write_text("pragma solidity ^0.8.20; contract C {}")
    return [str(COMPILER), "--source", str(source), "--output-dir", str(tmp_path / "out"),
            "--log-level", "error", "--no-output-logs",
            *(["--puya-path", str(backend)] if backend else ["--no-puya"])]


@pytest.mark.parametrize("stop", [signal.SIGINT, signal.SIGTERM])
def test_cli_cancellation_reaps_backend_group(tmp_path, stop):
    if not sys.platform.startswith("linux"):
        pytest.skip("Linux process/subreaper regression")
    libc = ctypes.CDLL(None)
    previous = ctypes.c_int()
    assert libc.prctl(37, ctypes.byref(previous), 0, 0, 0) == 0
    assert libc.prctl(36, 1, 0, 0, 0) == 0
    backend = tmp_path / "backend"
    backend.write_text("#!/usr/bin/env python3\nimport os, signal\n"
                       "child = os.fork()\n"
                       "if child: print(os.getpid(), child, flush=True)\n"
                       "while True: signal.pause()\n")
    backend.chmod(0o755)
    process = subprocess.Popen(command(tmp_path, backend), stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    parent = child = None
    try:
        assert select.select([process.stdout], [], [], 30)[0], "backend did not start"
        parent, child = map(int, process.stdout.readline().split())
        assert os.getpgid(parent) == parent
        os.killpg(process.pid, stop)
        assert process.wait(timeout=10) == 128 + stop
        with pytest.raises(ProcessLookupError):
            os.kill(parent, 0)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            reaped, status = os.waitpid(child, os.WNOHANG)
            if reaped:
                assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL
                child = None
                break
            time.sleep(0.01)
        assert child is None, "backend descendant survived cancellation"
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=10)
        if parent:
            try:
                os.killpg(parent, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for pid in (parent, child):
            if pid:
                try:
                    os.waitpid(pid, 0)
                except ChildProcessError:
                    pass
        libc.prctl(36, previous.value, 0, 0, 0)


def test_cli_rejects_backend_without_arc56(tmp_path):
    backend = tmp_path / "backend"
    backend.write_text("#!/usr/bin/env python3\nimport pathlib, sys\n"
                       "out = pathlib.Path(sys.argv[sys.argv.index('--awst') + 1]).parent\n"
                       "for suffix in ('.approval.bin', '.clear.bin', '.approval.teal', '.clear.teal'):\n"
                       "    (out / ('C' + suffix)).write_bytes(b'fresh')\n")
    backend.chmod(0o755)
    result = subprocess.run(command(tmp_path, backend), capture_output=True, text=True, timeout=30)
    assert result.returncode == 1, result.stderr
    assert "arc56.json" in result.stderr
    assert json.loads((tmp_path / "out/artifact-manifest.json").read_text())["phase"] == "frontend-ready"


@pytest.mark.parametrize("legacy", [False, True])
def test_legacy_import_alias_does_not_erase_local_event(tmp_path, legacy):
    args = command(tmp_path)
    (tmp_path / "I.sol").write_text("pragma solidity ^0.8.20; interface I { event E(uint value); }")
    (tmp_path / "C.sol").write_text('pragma solidity ^0.8.20; import {I as J} from "./I.sol"; '
                                  'contract I {} contract C is I { event E(uint value); '
                                  'function f() external { emit E(7); } }')
    result = subprocess.run(args + (["--legacy-source-rewrite"] if legacy else []),
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr


def test_shared_awst_and_immutability_match_pinned_backend(tmp_path):
    args = command(tmp_path)
    (tmp_path / "C.sol").write_text("pragma solidity ^0.8.20; contract C { "
        "function f(uint[] memory a) public pure returns(uint[] memory, uint) { return (a, a.length); } }")
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    script = r'''
import copy, json, sys
from puya.awst.serialize import awst_from_json, awst_to_json, get_converter
from puya.awst import wtypes
raw = json.load(open(sys.argv[1]))
definitions, references, tuples = {}, set(), []
def gather(value):
    if isinstance(value, dict):
        if '_$%!#ID' in value:
            assert value['_$%!#ID'] not in definitions
            definitions[value['_$%!#ID']] = value
        if '_$%!#REF' in value: references.add(value['_$%!#REF'])
        if value.get('_type') in ('WTuple', 'ARC4Tuple', 'ARC4Struct'): tuples.append(value)
        for child in value.values(): gather(child)
    elif isinstance(value, list):
        for child in value: gather(child)
gather(raw)
assert references and references <= definitions.keys()
assert tuples and any(not item['immutable'] for item in tuples)
for item in tuples:
    assert get_converter().structure(copy.deepcopy(item), wtypes.WType).immutable == item['immutable']
def expand(value):
    if isinstance(value, dict):
        if '_$%!#REF' in value: return expand(definitions[value['_$%!#REF']])
        return {key: expand(child) for key, child in value.items() if key != '_$%!#ID'}
    if isinstance(value, list): return [expand(child) for child in value]
    return value
compact = awst_from_json(json.dumps(raw))
expanded = awst_from_json(json.dumps(expand(raw)))
assert awst_to_json(compact) == awst_to_json(expanded)
'''
    result = subprocess.run([str(PUYA.parent / "python"), "-c", script, str(tmp_path / "out/awst.json")],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
