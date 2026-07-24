#!/usr/bin/env python3
"""PRECOMPILE-I/O differential campaign — memory-in / staticcall / memory-out.

Precompiles read input from memory and write output to memory; the AVM routes
staticcall(0x02/0x04/0x05, …) to native handlers with slot-routed I/O (M7/M8/
M10). This stresses that round-trip for the DETERMINISTIC precompiles —
  0x02 SHA-256   (arbitrary input → 32-byte digest),
  0x04 Identity  (input → identical output),
  0x05 ModExp    (32/32/32 layout → base^exp mod m),
at varied input sizes and output offsets INCLUDING slot-crossing (>4096) —
diffing the output words vs live solc+py-evm. ecRecover (0x01) and BN254
(0x06/07/08) need valid signatures / curve points → out of scope (random input
reverts on both, uninformative).

Usage: python fuzz_precompile.py [--contracts N] [--seed S] [--max-per-fn N]
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

OUT_OFFS = [0x100, 0x120, 0x200, 0x1000, 0x1800]  # incl. slot-crossing


def gen_contract(seed):
    rng = random.Random(seed)
    fns = []
    fi = 0

    def sha_fn(nwords):
        nonlocal fi
        params = ", ".join(f"uint256 a{k}" for k in range(nwords))
        stores = "\n            ".join(f"mstore({0x80 + 32*k}, a{k})" for k in range(nwords))
        outoff = rng.choice(OUT_OFFS)
        f = f"""
    function sha{fi}({params}) external view returns (bytes32 h) {{
        assembly {{
            {stores}
            let ok := staticcall(gas(), 0x2, 0x80, {32*nwords}, {outoff}, 32)
            if iszero(ok) {{ revert(0, 0) }}
            h := mload({outoff})
        }}
    }}"""
        fi += 1
        return f

    def ident_fn(nwords):
        nonlocal fi
        params = ", ".join(f"uint256 a{k}" for k in range(nwords))
        stores = "\n            ".join(f"mstore({0x80 + 32*k}, a{k})" for k in range(nwords))
        outoff = rng.choice(OUT_OFFS)
        reads = "\n            ".join(f"r{k} := mload({outoff + 32*k})" for k in range(nwords))
        rets = ", ".join(f"bytes32 r{k}" for k in range(nwords))
        f = f"""
    function id{fi}({params}) external view returns ({rets}) {{
        assembly {{
            {stores}
            let ok := staticcall(gas(), 0x4, 0x80, {32*nwords}, {outoff}, {32*nwords})
            if iszero(ok) {{ revert(0, 0) }}
            {reads}
        }}
    }}"""
        fi += 1
        return f

    def modexp_fn():
        nonlocal fi
        outoff = rng.choice(OUT_OFFS)
        # 32/32/32 layout: lengths then base/exp/mod. Bound exp small-ish via mask
        # so square-and-multiply stays in budget; mod forced odd+>1.
        f = f"""
    function me{fi}(uint256 base, uint256 e, uint256 m) external view returns (bytes32 r) {{
        assembly {{
            mstore(0x80, 32) mstore(0xa0, 32) mstore(0xc0, 32)
            mstore(0xe0, base)
            mstore(0x100, and(e, 0xffff))
            mstore(0x120, or(or(m, 1), 3))
            let ok := staticcall(gas(), 0x5, 0x80, 0xc0, {outoff}, 32)
            if iszero(ok) {{ revert(0, 0) }}
            r := mload({outoff})
        }}
    }}"""
        fi += 1
        return f

    for _ in range(rng.randrange(2, 4)):
        fns.append(sha_fn(rng.randrange(1, 4)))
    for _ in range(rng.randrange(1, 3)):
        fns.append(ident_fn(rng.randrange(1, 3)))
    for _ in range(rng.randrange(1, 3)):
        fns.append(modexp_fn())

    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_precompile.py seed={seed} — precompile-I/O differential fixture (generated).
contract PreFuzz {{
{''.join(fns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 60)
    seed0 = opt("--seed", 10000)
    max_per_fn = opt("--max-per-fn", 6)

    outdir = HERE / "out_precompile"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"pre_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            # precompiles (modexp square-and-multiply) need opcode budget.
            r = run_stateful_diff(fixture, entry="PreFuzz", max_per_fn=max_per_fn,
                                  budget_pool=20, harness=harness, quiet=True)
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
    print(f"PRECOMPILE CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
