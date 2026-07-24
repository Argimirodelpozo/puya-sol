#!/usr/bin/env python3
"""STATEFUL differential fuzzer — like fuzz_evm.py but storage PERSISTS across a call SEQUENCE.

Both sides keep one deployed instance and commit state between calls:
  - EVM oracle (stateful=True): view/pure → .call(); state-changing → .call() (return) + .transact() (commit).
  - AVM harness: view/pure → simulate (expect_revert=True, reads committed state); state-changing →
    execute (default expect_revert=False, commits a real txn; revert => state unchanged).

The per-function fuzzed calls are INTERLEAVED round-robin so getters observe the running state after
intermediate mutations. A divergence means the persisted state evolved differently on AVM vs EVM.

Usage: python fuzz_state.py contracts/<fixture>.sol [--max-per-fn N]
"""
import sys
from collections import OrderedDict, deque
from pathlib import Path

from fuzz_evm import (HERE, REVERT, _oracle, canon, _fmt, _fmt1,
                      gen_rows, _args_to_algo, _apply_addr_canon, Harness, LocalNet)
from event_diff import decode_avm_logs as _decode_avm_logs, logs_match as _logs_match
from revert_diff import revert_match as _revert_match


import re as _re


def _gather_sources(entry):
    """Transitively collect the .sol closure imported by `entry` (resolving relative
    import paths on disk), keyed by path relative to their common root so solc's
    relative-import resolution matches. Unresolvable imports (remappings) are
    skipped."""
    seen = {}                                            # abs Path -> content
    stack = [entry.resolve()]
    imp_re = _re.compile(r'import\s+(?:[^;"\']*\bfrom\s+)?["\']([^"\']+)["\']')
    while stack:
        f = stack.pop()
        if f in seen:
            continue
        try:
            txt = f.read_text(errors="replace")
        except Exception:
            continue
        seen[f] = txt
        for m in imp_re.finditer(txt):
            target = (f.parent / m.group(1)).resolve()
            if target.exists():
                stack.append(target)
    if len(seen) <= 1:
        return {"fixture.sol": seen.get(entry.resolve(), entry.read_text(errors="replace"))}
    import os
    root = Path(os.path.commonpath([str(p) for p in seen]))
    if root.is_file():
        root = root.parent
    out = {}
    for p, txt in seen.items():
        try:
            out[str(p.relative_to(root))] = txt
        except ValueError:
            out[p.name] = txt
    return out


def gen_state_calls(fns, max_per_fn):
    """Like fuzz_evm.gen_calls but KEEPS void (state-changing) functions — they mutate storage;
    the divergence is observed on subsequent getters, not their own (empty) return. Params can be
    scalars, arrays, or tuples/structs (gen_rows handles all)."""
    calls, skipped = [], []
    for fn in fns:
        sig, ins = fn["sig"], fn["inputs"]
        if not ins:
            calls.append((sig, [])); continue
        rows = gen_rows(ins, max_per_fn)
        if rows is None:
            skipped.append((sig, "non-fuzzable params")); continue
        for row in rows:
            calls.append((sig, row))
    return calls, skipped


def _is_view(mut):
    return mut in ("view", "pure")


def avm_step(h, app, sig, args, is_view):
    """Returns (value_or_REVERT, fail_reason_str, result_or_None). The Result
    carries the raw execute response for event-log decoding (mutators only)."""
    args = _args_to_algo(args)                                 # address markers → algosdk base32
    if is_view:
        r = h.call(app, sig, *args, expect_revert=True)        # simulate: read committed state
        return (REVERT, str(getattr(r, "fail_message", "")), r) if r.reverted else (r.abi_return, "", r)
    try:
        r = h.call(app, sig, *args)                            # execute: commit a real txn
        if getattr(r, "reverted", False):
            return REVERT, str(getattr(r, "fail_message", "") or getattr(r, "raw_response", "")), r
        return r.abi_return, "", r
    except Exception as e:
        return REVERT, str(e)[:200], None                      # reverted txn => state unchanged


# AVM platform limits (opcode budget past the 16-txn pool, box-reference packing):
# a revert for these reasons is NOT a miscompile — but it FORKS the state vs the
# EVM run, so the rest of the sequence is unverifiable.
def _is_platform_limit(reason: str) -> bool:
    m = (reason or "").lower()
    return ("budget" in m or "opcode" in m or "dynamic cost" in m
            or "invalid box reference" in m or "unavailable box" in m
            or "unavailable resource" in m or "max_group_size" in m
            or "exceed" in m and "group" in m)


def run_stateful_diff(fixture, entry=None, max_per_fn=24, budget_pool=0,
                      harness=None, quiet=False, solc_version="0.8.26",
                      evm_version="paris"):
    """Run the stateful differential (EVM oracle vs AVM harness) on one fixture.

    Returns a result dict:
      {ok, diffed, diverged: [(sig,args,exp,act)], avm_errors, evm_skips,
       contract, n_calls} — ok is True iff no divergences AND no AVM errors.
    Reused by main() (CLI) and by fuzz_mutate.py (corpus mutation campaign).
    Pass a shared `harness` to amortise LocalNet setup across many mutants.
    `quiet=True` suppresses the per-fixture prints (campaign mode)."""
    def say(*a):
        if not quiet:
            print(*a)

    base = {"fixture": str(fixture), "solc_version": solc_version, "evm_version": evm_version}
    if entry:
        base["contract"] = entry
    # Multi-file: pass the TRANSITIVELY-imported project sources so the EVM oracle
    # resolves `import`s (puya-sol resolves them natively from the real path).
    # Gathering only the imported closure (not the whole tree) avoids pulling in
    # unrelated files that don't compile. Keyed relative to the closure's common
    # root so relative (`./`, `../`) imports resolve. Imports that don't resolve on
    # disk (remappings like @openzeppelin/…) are skipped → compile-fails cleanly.
    if _re.search(r'(?m)^\s*import\b', fixture.read_text(errors="replace")):
        base["sources"] = _gather_sources(fixture)
    say(f"[introspect] {fixture.name}…")
    info = _oracle({**base, "introspect": True})
    # Pin the EVM-chosen entry contract so both sides deploy the SAME one.
    if not entry:
        entry = info.get("contract")
    if entry:
        base["contract"] = entry
    mut = {f["sig"]: f.get("mut", "") for f in info["functions"]}
    has_output = {f["sig"]: bool(f["outputs"]) for f in info["functions"]}
    outs_by_sig = {f["sig"]: f["outputs"] for f in info["functions"]}
    calls, skipped = gen_state_calls(info["functions"], max_per_fn)
    if not calls:
        sys.exit("no fuzzable functions")

    # Sequence so every zero-arg view getter is RE-READ after each state-changing call — a getter
    # enqueued once would only observe the state at its single round-robin slot (often the INITIAL
    # state, missing every later mutation; this exact gap hid the int16 struct-getter bug — see
    # [[differential-fuzzing-spike]]). Param-bearing getters/mutators interleave round-robin.
    zero_getters = [(f["sig"], []) for f in info["functions"]
                    if _is_view(f.get("mut", "")) and not f["inputs"] and f["outputs"]]
    zero_sigs = {s for s, _ in zero_getters}
    work = OrderedDict()
    for sig, args in calls:
        if sig in zero_sigs:
            continue                              # resampled below, not via the work queue
        work.setdefault(sig, deque()).append((sig, args))
    seq = []
    while any(work.values()):
        for sig in list(work):
            if work[sig]:
                seq.append(work[sig].popleft())
                seq.extend(zero_getters)          # re-read all zero-arg getters after each step
    if not seq:                                   # only zero-arg getters present → read each once
        seq = list(zero_getters)

    n_mut = sum(1 for s, _ in seq if not _is_view(mut.get(s, "")))
    say(f"  contract {info['contract']}: {len(seq)} calls in sequence "
        f"({n_mut} state-changing, {len(seq) - n_mut} reads)")

    say(f"[EVM] running stateful sequence…")
    evm_resp = _oracle({**base, "stateful": True, "logs": True,
                        "calls": [{"sig": s, "args": a} for s, a in seq]})
    evm = evm_resp["results"]

    if harness is None:
        ln = LocalNet(); harness = Harness(ln, HERE / "out_state")
    say("[AVM] compiling + deploying…")
    app = harness.compile_and_deploy(fixture, contract_name=entry, postinit_budget_pool=budget_pool)
    avm_events = getattr(app.app_spec, "events", None) or []
    # msg.sender/deployer AND address(this) differ between the two chains by
    # construction; map each side's own to a shared sentinel so contracts that
    # return owner = msg.sender or address(this) don't false-diverge.
    from algosdk import encoding as _enc
    _evm_map = {(evm_resp.get("caller") or "").lower(): "0xCALLER",
                (evm_resp.get("self") or "").lower(): "0xSELF"}
    _avm_map = {("0x" + _enc.decode_address(harness.localnet.account.address).hex()).lower(): "0xCALLER",
                ("0x" + _enc.decode_address(app.app_addr).hex()).lower(): "0xSELF"}
    _evm_map.pop("", None); _avm_map.pop("", None)

    def _sub_addrs(v, m):
        if isinstance(v, str) and v.lower() in m:
            return m[v.lower()]
        if isinstance(v, (list, tuple)):
            return [_sub_addrs(x, m) for x in v]
        return v

    diverged, avm_errors, evm_skips, limit_fork = [], [], [], None
    event_div, revert_div = [], []
    for (sig, args), er in zip(seq, evm):
        if er.get("ok"):
            expected = er["value"]
        elif er.get("revert"):
            expected = REVERT
        else:
            evm_skips.append((sig, args, er.get("err", "?"))); continue
        try:
            actual, avm_reason, avm_res = avm_step(harness, app, sig, args, _is_view(mut.get(sig, "")))
        except Exception as e:
            avm_errors.append((sig, args, type(e).__name__ + ": " + str(e)[:140])); continue
        # AVM platform limit (opcode budget / box-ref packing) where EVM succeeded:
        # not a miscompile, but the state has now FORKED from the EVM run — stop
        # diffing; everything after is unverifiable.
        if actual == REVERT and expected != REVERT and _is_platform_limit(avm_reason):
            limit_fork = (sig, args, avm_reason[:140])
            break
        if has_output.get(sig):
            outs = outs_by_sig.get(sig, [])
            # address returns → 32-byte content hex, then map each side's own caller
            # (msg.sender/deployer) to a shared sentinel.
            exp_c = _sub_addrs(_apply_addr_canon(expected, outs), _evm_map)
            act_c = _sub_addrs(_apply_addr_canon(actual, outs), _avm_map)
            if canon(act_c) != canon(exp_c):
                diverged.append((sig, args, exp_c, act_c))
        else:
            # Void mutator: the return is []/None noise — only the REVERT STATUS matters
            # (a mutator reverting on one side but committing on the other desyncs the sequence).
            if (expected == REVERT) != (actual == REVERT):
                diverged.append((sig, args,
                                 "REVERT" if expected == REVERT else "ok",
                                 "REVERT" if actual == REVERT else "ok"))
        # EVENT-LOG diff: only when BOTH sides ran (no revert on either) — a revert
        # emits nothing, and a revert-status divergence is already reported above.
        if actual != REVERT and expected != REVERT and "logs" in er:
            # abi_results lives on the AtomicTransactionResponse (avm_res is a
            # Result wrapping it) — pass the raw_response, else decode_avm_logs
            # silently returns None and EVENT DIFFING IS SKIPPED (was: always
            # skipped, so event divergences went invisible).
            avm_logs = _decode_avm_logs(
                getattr(avm_res, "raw_response", avm_res), avm_events)
            if avm_logs is not None:
                ok, evm_only, avm_only = _logs_match(er["logs"], avm_logs)
                if not ok:
                    event_div.append((sig, args, evm_only, avm_only))
        # REVERT-PAYLOAD diff: only when BOTH reverted (status match already handled
        # above). Compares revert KIND + Error(string) message; tolerates the
        # documented keccak/sha512_256 + backing-width + Panic-not-emitted conventions.
        if actual == REVERT and expected == REVERT and "revert_data" in er:
            avm_rd = getattr(avm_res, "revert_data", None) if avm_res is not None else None
            ok, ev_d, av_d = _revert_match(er["revert_data"], avm_rd)
            if not ok:
                revert_div.append((sig, args, ev_d, av_d))

    diffed = len(seq) - len(avm_errors) - len(evm_skips)
    say(f"\n=== {diffed} sequenced calls diffed (AVM vs live EVM, state persisted) ===")
    if avm_errors:
        say(f"\n⚠️  {len(avm_errors)} AVM errors:")
        for sig, args, err in avm_errors[:8]:
            say(f"   {sig}{_fmt(args)}  {err}")
    if limit_fork:
        say(f"\n⏭️  AVM platform limit at {limit_fork[0]}{_fmt(limit_fork[1])} — "
            f"state forked, sequence truncated: {limit_fork[2]}")
    if diverged:
        say(f"\n❌ {len(diverged)} DIVERGENCE(S):")
        for sig, args, exp, act in diverged[:25]:
            say(f"   {sig}{_fmt(args)}  evm={_fmt1(exp)}  avm={_fmt1(act)}")
    if event_div:
        say(f"\n📣 {len(event_div)} EVENT DIVERGENCE(S):")
        for sig, args, evm_only, avm_only in event_div[:25]:
            say(f"   {sig}{_fmt(args)}  evm_only={evm_only}  avm_only={avm_only}")
    if revert_div:
        say(f"\n💥 {len(revert_div)} REVERT-PAYLOAD DIVERGENCE(S):")
        for sig, args, ev_d, av_d in revert_div[:25]:
            say(f"   {sig}{_fmt(args)}  evm={ev_d}  avm={av_d}")
    if not diverged and not event_div and not revert_div and not quiet:
        say("\n✅ no divergences — state + events + revert payloads match a live solc+EVM")
    return {"ok": not diverged and not event_div and not revert_div and not avm_errors,
            "diffed": diffed, "diverged": diverged, "event_div": event_div,
            "revert_div": revert_div,
            "avm_errors": avm_errors, "evm_skips": evm_skips,
            "limit_fork": limit_fork,
            "contract": info["contract"], "n_calls": len(seq)}


def main():
    argv = list(sys.argv[1:])
    max_per_fn = 24
    if "--max-per-fn" in argv:
        i = argv.index("--max-per-fn"); max_per_fn = int(argv[i + 1]); del argv[i:i + 2]
    # --contract NAME pins the ENTRY contract on BOTH sides (EVM oracle + AVM harness). Needed for
    # multi-contract fixtures (e.g. cross-contract: a Caller that `new`s a Callee) where the default
    # "most functions" heuristic could pick the wrong one and every call would 404 → spurious REVERTs.
    entry = None
    if "--contract" in argv:
        i = argv.index("--contract"); entry = argv[i + 1]; del argv[i:i + 2]
    # --budget-pool N: deploy-time opcode pool (cross-contract inner-creates can need it).
    budget_pool = 0
    if "--budget-pool" in argv:
        i = argv.index("--budget-pool"); budget_pool = int(argv[i + 1]); del argv[i:i + 2]
    fixture = Path(argv[0]).resolve()
    r = run_stateful_diff(fixture, entry=entry, max_per_fn=max_per_fn, budget_pool=budget_pool)
    # Exit non-zero on divergence so callers (fuzz_crosscall, campaign) actually see it.
    sys.exit(1 if r["diverged"] else 0)


if __name__ == "__main__":
    main()
