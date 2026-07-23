#!/usr/bin/env python3
"""MEMORY-OPS differential campaign — the scratch-slot EVM-memory model.

EVM linear memory is modelled as AVM scratch-slot blobs (SLOT_SIZE=4096). This
stresses the raw op set — mstore / mstore8 / mload / mcopy / calldatacopy — at
explicit offsets, including SLOT-CROSSING (>4096) and unaligned ones, plus
overlapping mcopy (memmove, M13) and calldatacopy tail zero-pad (M12). Each
generated function does a straight-line write sequence then reads words back and
returns them as bytes32 — a per-call differential vs live solc+py-evm (memory
is fresh per call, so these are self-contained stateless probes).

DELIBERATELY scalar-param + explicit-offset only: `bytes memory` params
referenced in asm and returned are the SEPARATE known-open memory-param-return
seam (test_mcopy) — out of scope here. calldata source is a `bytes calldata`
param (item-2 path), not memory.

mcopy is a Cancun opcode → solc 0.8.26 + evm_version cancun.

Usage: python fuzz_mem.py [--contracts N] [--seed S] [--max-per-fn N]
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# Bounded offsets: aligned, unaligned, and slot-crossing (0x1000 = slot 1).
OFFSETS = [0x80, 0xa0, 0xc0, 0xe0, 0x100, 0x108, 0x120, 0x200,
           0xff0, 0x1000, 0x1010, 0x1020, 0x1800]
LENS = [8, 16, 24, 32, 40, 48, 64, 33, 31, 65]


def gen_contract(seed):
    rng = random.Random(seed)
    fns = []
    for fi in range(rng.randrange(4, 8)):
        writes, reads = [], []
        # 3-6 write ops
        for _ in range(rng.randrange(3, 7)):
            k = rng.random()
            if k < 0.4:
                off = rng.choice(OFFSETS)
                val = rng.choice(["a", "b", "c", "add(a,b)", "xor(a,c)", str(rng.randrange(1 << 32))])
                writes.append(f"mstore({off}, {val})")
            elif k < 0.6:
                off = rng.choice(OFFSETS)
                writes.append(f"mstore8({off}, and({rng.choice(['a','b','c'])}, 0xff))")
            elif k < 0.85:
                dst, src = rng.choice(OFFSETS), rng.choice(OFFSETS)
                ln = rng.choice(LENS)
                writes.append(f"mcopy({dst}, {src}, {ln})")
            else:
                dst = rng.choice(OFFSETS)
                ln = rng.choice([4, 8, 16, 32, 48])
                # copy from calldata: src past the head so tail zero-pads (M12)
                src = rng.choice([4, 0x24, 0x44])  # skip selector[0:4] (design divergence)
                writes.append(f"calldatacopy({dst}, {src}, {ln})")
        # 2-4 reads at written/nearby offsets
        rn = rng.randrange(2, 5)
        roffs = rng.sample(OFFSETS, min(rn, len(OFFSETS)))
        for j, off in enumerate(roffs):
            reads.append((f"r{j}", off))
        ret_decl = ", ".join(f"bytes32 r{j}" for j in range(len(roffs)))
        write_src = "\n            ".join(writes)
        read_src = "\n            ".join(f"r{j} := mload({off})" for j, off in enumerate(roffs))
        fns.append(f"""
    function m{fi}(uint256 a, uint256 b, uint256 c, bytes calldata cd)
        external pure returns ({ret_decl})
    {{
        assembly {{
            {write_src}
            {read_src}
        }}
        cd;
    }}""")
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_mem.py seed={seed} — memory-ops differential fixture (generated).
contract MemFuzz {{
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
    seed0 = opt("--seed", 20000)
    max_per_fn = opt("--max-per-fn", 8)

    outdir = HERE / "out_mem"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"mem_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            # mcopy is Cancun.
            r = run_stateful_diff(fixture, entry="MemFuzz", max_per_fn=max_per_fn,
                                  harness=harness, quiet=True,
                                  solc_version="0.8.26", evm_version="cancun")
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
        n_div = len(r["diverged"]) + len(r["revert_div"])
        if n_div or r["avm_errors"]:
            findings.append((seed, r))
            print(f"  ❌ seed {seed}: {len(r['diverged'])} value / "
                  f"{len(r['revert_div'])} revert / {len(r['avm_errors'])} AVM-err")
            for sig, args, exp, act in r["diverged"][:8]:
                print(f"     {sig}{tuple(args)}  evm={exp}  avm={act}")
            for sig, args, err in r["avm_errors"][:4]:
                print(f"     AVM-ERR {sig}{tuple(args)}: {err}")
        else:
            print(f"  ✅ {r['diffed']} calls clean")

    print("\n" + "=" * 60)
    print(f"MEM CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
