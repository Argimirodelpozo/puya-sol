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
    args = _args_to_algo(args)                                 # address markers → algosdk base32
    if is_view:
        r = h.call(app, sig, *args, expect_revert=True)        # simulate: read committed state
        return REVERT if r.reverted else r.abi_return
    try:
        r = h.call(app, sig, *args)                            # execute: commit a real txn
        return REVERT if getattr(r, "reverted", False) else r.abi_return
    except Exception:
        return REVERT                                          # reverted txn => state unchanged


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
    say(f"[introspect] {fixture.name}…")
    info = _oracle({**base, "introspect": True})
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
    evm = _oracle({**base, "stateful": True,
                   "calls": [{"sig": s, "args": a} for s, a in seq]})["results"]

    if harness is None:
        ln = LocalNet(); harness = Harness(ln, HERE / "out_state")
    say("[AVM] compiling + deploying…")
    app = harness.compile_and_deploy(fixture, contract_name=entry, postinit_budget_pool=budget_pool)

    diverged, avm_errors, evm_skips = [], [], []
    for (sig, args), er in zip(seq, evm):
        if er.get("ok"):
            expected = er["value"]
        elif er.get("revert"):
            expected = REVERT
        else:
            evm_skips.append((sig, args, er.get("err", "?"))); continue
        try:
            actual = avm_step(harness, app, sig, args, _is_view(mut.get(sig, "")))
        except Exception as e:
            avm_errors.append((sig, args, type(e).__name__ + ": " + str(e)[:140])); continue
        if has_output.get(sig):
            outs = outs_by_sig.get(sig, [])
            exp_c = _apply_addr_canon(expected, outs)       # address returns → 32-byte content hex
            act_c = _apply_addr_canon(actual, outs)
            if canon(act_c) != canon(exp_c):
                diverged.append((sig, args, exp_c, act_c))
        else:
            # Void mutator: the return is []/None noise — only the REVERT STATUS matters
            # (a mutator reverting on one side but committing on the other desyncs the sequence).
            if (expected == REVERT) != (actual == REVERT):
                diverged.append((sig, args,
                                 "REVERT" if expected == REVERT else "ok",
                                 "REVERT" if actual == REVERT else "ok"))

    diffed = len(seq) - len(avm_errors) - len(evm_skips)
    say(f"\n=== {diffed} sequenced calls diffed (AVM vs live EVM, state persisted) ===")
    if avm_errors:
        say(f"\n⚠️  {len(avm_errors)} AVM errors:")
        for sig, args, err in avm_errors[:8]:
            say(f"   {sig}{_fmt(args)}  {err}")
    if diverged:
        say(f"\n❌ {len(diverged)} DIVERGENCE(S):")
        for sig, args, exp, act in diverged[:25]:
            say(f"   {sig}{_fmt(args)}  evm={_fmt1(exp)}  avm={_fmt1(act)}")
    elif not quiet:
        say("\n✅ no divergences — persisted state matches a live solc+EVM across the sequence")
    return {"ok": not diverged and not avm_errors, "diffed": diffed,
            "diverged": diverged, "avm_errors": avm_errors, "evm_skips": evm_skips,
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
