#!/usr/bin/env python3
"""EVENT-EMISSION differential campaign — emit with scalar + AGGREGATE args.

puya-sol lowers `emit E(args)` to log(selector(4B) ++ ARC4-tuple(args)). The
standalone fuzz_events differ is SCALAR-only per-fixture; the campaign
generators never emitted events at all. This generates contracts that emit
events with mixed arg types — scalars (signed/unsigned sub-word, bool, bytesN,
address) AND static aggregates (fixed arrays, structs), the documented gap —
driven by fuzzed inputs, and diffs the emitted logs against live solc+py-evm via
run_stateful_diff (event_diff decodes the ARC4 tuple body; static aggregates
tuple-decode, dynamic ones it skips). Now also generates VALUE-type `indexed`
params (up to 3/event): the indexed keyword is a no-op on topic-less AVM, so all
args ride in the ARC-28 tuple and match EVM (which stores value types in the
topic). Indexed DYNAMIC types are a documented divergence (EVM keccak-hashes them;
puya-sol keeps the value) and are NOT generated — see indexed-event-params memory.

Usage: python fuzz_emit.py [--contracts N] [--seed S] [--max-per-fn N]
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

SCALARS = ["uint8", "int8", "uint16", "int16", "uint32", "int64", "uint128",
           "uint256", "int256", "bool", "bytes4", "bytes32", "address"]
STATIC_AGGS = ["uint8[3]", "int16[2]", "uint256[2]", "bytes4[2]"]


def gen_contract(seed):
    rng = random.Random(seed)
    # one shared static struct for struct-arg events
    struct_def = "struct P { uint64 x; int16 y; bool z; bytes4 w; }"

    events, fns = [], []
    for i in range(rng.randrange(4, 8)):
        n = rng.randrange(1, 4)
        pool = SCALARS + STATIC_AGGS
        types = [rng.choice(pool) for _ in range(n)]
        # event def — mark up to 3 VALUE-TYPE scalar params `indexed` (EVM stores
        # value types directly in the topic → matches AVM's all-in-tuple emit; the
        # `indexed` keyword is a no-op on topic-less AVM). Indexed DYNAMIC/aggregate
        # types are a documented divergence (EVM keccak-hashes them into the topic;
        # puya-sol keeps the value) and are deliberately NOT generated here.
        idx_left = 3
        ev_parts = []
        for j, t in enumerate(types):
            ix = ""
            if t in SCALARS and idx_left > 0 and rng.random() < 0.5:
                ix = " indexed"; idx_left -= 1
            ev_parts.append(f"{t}{ix} v{j}")
        ev_params = ", ".join(ev_parts)
        events.append(f"    event E{i}({ev_params});")
        # emit fn: aggregate/struct args come in as calldata/memory params
        fn_params, emit_args = [], []
        for j, t in enumerate(types):
            if t in STATIC_AGGS:
                fn_params.append(f"{t} calldata v{j}")
            elif t == "P":
                fn_params.append(f"P calldata v{j}")
            else:
                fn_params.append(f"{t} v{j}")
            emit_args.append(f"v{j}")
        fns.append(f"""
    function emit{i}({", ".join(fn_params)}) external {{
        emit E{i}({", ".join(emit_args)});
    }}""")

    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_emit.py seed={seed} — event-emission differential fixture (generated).
contract EmitFuzz {{
    {struct_def}
{chr(10).join(events)}
{''.join(fns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 80)
    seed0 = opt("--seed", 5000)
    max_per_fn = opt("--max-per-fn", 6)

    outdir = HERE / "out_emit"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"emit_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="EmitFuzz", max_per_fn=max_per_fn,
                                  harness=harness, quiet=True)
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            msg = str(e)[:200]
            if "solc" in msg.lower() or "compil" in msg.lower():
                skips += 1
                print(f"  ~ skip: {msg[:110]}")
            else:
                errors.append((seed, type(e).__name__ + ": " + msg))
                print(f"  ⚠️ runner error: {errors[-1][1]}")
            continue
        n_div = len(r["diverged"]) + len(r["event_div"]) + len(r["revert_div"])
        if n_div or r["avm_errors"]:
            findings.append((seed, r))
            print(f"  ❌ seed {seed}: {len(r['diverged'])} value / {len(r['event_div'])} EVENT / "
                  f"{len(r['revert_div'])} revert / {len(r['avm_errors'])} AVM-err")
            for sig, args, evm_only, avm_only in r["event_div"][:8]:
                print(f"     EVENT {sig}{tuple(args)}  evm_only={evm_only}  avm_only={avm_only}")
            for sig, args, err in r["avm_errors"][:4]:
                print(f"     AVM-ERR {sig}{tuple(args)}: {err}")
        else:
            print(f"  ✅ {r['diffed']} calls clean")

    print("\n" + "=" * 60)
    print(f"EMIT CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['event_div'])}e/"
              f"{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
