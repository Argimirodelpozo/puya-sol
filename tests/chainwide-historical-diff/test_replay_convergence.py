"""An exhausted retry loop must not compare asymmetric states."""
import json

import pytest

import replay


@pytest.mark.parametrize("converges", [False, True])
def test_platform_skip_convergence_is_required(monkeypatch, tmp_path, converges):
    runs, comparisons = [], []

    def load(path):
        if path.name == "case.json":
            return {"txns": []}
        if path.name == "evm_results.json":
            return {"time_base": 100, "deployment_time": 100}
        passes = len(runs) // 2
        return {"platform_limits": {} if converges and passes == 2
                else {str(passes): "opcode budget exhausted"}}

    monkeypatch.setattr(replay, "CASES", tmp_path)
    monkeypatch.setattr(replay, "load_json", load)
    monkeypatch.setattr(replay, "_chain_now", lambda: 100)
    monkeypatch.setattr(replay, "_run", lambda cmd, tag: runs.append((tag, json.loads(cmd[-1]))))
    monkeypatch.setattr(replay, "diff_case", lambda path: comparisons.append(path) or {"done": True})
    if converges:
        assert replay.replay("case") == {"done": True}
        assert len(runs) == 4 and len(comparisons) == 1
        assert runs[2][1]["skips"] == {"1": "avm-platform-limit:opcode budget exhausted"}
        assert runs[3][1]["skips"] == ["1"]
    else:
        with pytest.raises(RuntimeError, match="states are asymmetric"):
            replay.replay("case")
        assert len(runs) == 14 and not comparisons
