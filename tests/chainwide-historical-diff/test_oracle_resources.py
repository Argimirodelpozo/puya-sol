"""Resource retries must execute real helpers and never override a rejection."""
from copy import deepcopy
from types import SimpleNamespace

import pytest

import oracle_case
from chd_common import is_platform_limit
from oracle_case import MIN_FEE, OPUP_DEPTH, POOL, OracleLane


@pytest.mark.parametrize("budget_failures", [0, 1, 2])
def test_create_budget_retries_are_atomic_paid_groups(budget_failures):
    requests, committed, registered = [], [], []

    def run(request):
        requests.append(deepcopy(request))
        return ({"result": "PANIC", "error": "dynamic cost budget exceeded"}
                if len(requests) <= budget_failures else {"result": "ACCEPT"})

    lane = SimpleNamespace(
        state=SimpleNamespace(request=lambda source, **kw: {"source": source, **kw},
                              carry=committed.append, register_application=registered.append),
        oracle=SimpleNamespace(run=run), creator="00" * 32, approval="int 1",
        approval_bin=b"", clear_bin=b"", global_uints=16, global_bytes=16,
        extra_pages=0, write_budget_refs=2, round=1000, dep_apps=[], stats={},
        _app_fields=lambda: {"execute_group": True})
    lane._siblings = lambda *args: OracleLane._siblings(lane, *args)
    OracleLane.create(lane, ["abcd"], 123)
    assert len(requests) == budget_failures + 1
    assert len(committed) == len(registered) == 1
    assert lane.round == 1001
    assert lane.stats["create_helpers"] == (POOL if budget_failures else 0)
    assert "group" not in requests[0]
    for index, request in enumerate(requests[1:]):
        assert request["creating"] and request["execute_group"]
        assert request["app_args"] == ["abcd"]
        assert request["group_index"] == len(request["group"]) == POOL
        assert all(txn["type_enum"] == 6 for txn in request["group"])
        depth = OPUP_DEPTH if index else 0
        assert all(int(txn["app_args"][0], 16) == depth for txn in request["group"])
        assert request["fee"] >= MIN_FEE * (POOL + 1 + POOL * depth)
        assert not any("budget" in key for key in request)


def test_create_does_not_retry_or_commit_contract_rejection():
    class State:
        def request(self, source, **kw):
            return {"source": source, **kw}

        def carry(self, _response):
            pytest.fail("rejected creation must not commit")

    requests = []

    def reject(req):
        requests.append(req)
        return {"result": "REJECT", "error": "assert failed"}

    lane = SimpleNamespace(state=State(), oracle=SimpleNamespace(run=reject),
                           creator="00" * 32, approval="int 0", write_budget_refs=0,
                           round=1000, dep_apps=[], stats={}, _app_fields=lambda: {})
    with pytest.raises(RuntimeError, match="assert failed"):
        OracleLane.create(lane, [], 123)
    assert len(requests) == 1 and lane.round == 1000


@pytest.mark.parametrize("resolved", [False, True])
def test_box_errors_are_not_resource_exclusions_without_exhaustion(monkeypatch, resolved):
    monkeypatch.setattr(oracle_case, "RETRY_CAP", 3)
    count = 0

    def run(req):
        nonlocal count
        count += 1
        return {"result": "PANIC", "error": f"invalid Box reference 0x{count:064x}"}

    lane = SimpleNamespace(
        fund=lambda sender: None, memo=[], stats={"calls": 0, "attempts": 0, "discoveries": 0},
        build=lambda *args, **kwargs: {}, oracle=SimpleNamespace(run=run),
        _attribute=lambda key, found: (9001, key) if resolved else None)
    ok, response, info = OracleLane.call(lane, "00" * 32, [], ts=100)
    assert not ok
    assert count == (3 if resolved else 1)
    assert info["attempts"] == count
    assert response["replay_resources"] == info
    assert is_platform_limit(response["error"]) is resolved
    assert f"invalid Box reference 0x{count:064x}" in response["error"]
    if resolved:
        assert "resource discovery exhausted" in response["error"]
        assert info["refs"] == 3


@pytest.mark.parametrize("opcode", ["byte", "pushbytes", "int", "pushint"])
def test_unresolved_child_templates_are_an_explicit_unsupported_feature(opcode):
    with pytest.raises(NotImplementedError, match="real new C"):
        OracleLane(None, None, f"#pragma version 12\n{opcode} TMPL_APPROVAL_Silo_P0\n",
                   "int 1", b"", b"", "00" * 32)
