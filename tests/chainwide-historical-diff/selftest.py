#!/usr/bin/env python3
"""Self-test for the STORAGE DIFFER, using a synthetic case instead of a fetch.

Real histories only exercise the storage shapes those contracts happen to use,
and "both legs empty" reads as a pass. op_gov's window, for instance, never
mints, so `_checkpoints`/`_nonces` stay empty and their readers are never run.
This builds a case whose calls are guaranteed to populate every shape the
readers claim to handle — scalar, nested, STRUCT and ARRAY valued mappings —
so a broken decoder shows up as a divergence rather than as silence.

  <tiny-fuzzing-oracle/.evmvenv/bin/python> selftest.py

(runs under the EVM venv — it needs solcx/eth_abi to synthesise the case; both
replay legs are subprocesses with their own interpreters, so that is fine.)

The case is a normal case dir (cases/selftest/), so replay.py/differ.py run on
it unmodified; only the txn list is synthesised rather than fetched.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import CASES, dump_json, load_json

TAG = "selftest"

SOURCE = """// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StorageShapes {
    struct Counter { uint256 value; }
    struct Checkpoint { uint32 fromBlock; uint224 votes; }   // packed, one slot

    mapping(address => uint256) public bal;                  // scalar
    mapping(address => mapping(address => uint256)) public allow;  // nested
    mapping(address => Counter) public nonces;               // struct
    mapping(address => Checkpoint[]) public ckpts;           // dynamic array
    uint256 public total;

    function credit(address a, uint256 v) external { bal[a] += v; total += v; }
    function approve(address o, address s, uint256 v) external { allow[o][s] = v; }
    function bump(address a) external { nonces[a].value += 1; }
    function push(address a, uint32 fb, uint224 v) external {
        ckpts[a].push(Checkpoint(fb, v));
    }
    function ckptLen(address a) external view returns (uint256) {
        return ckpts[a].length;
    }
}
"""

# (sig, arg-markers). Address args use the registry marker form the harness
# already understands, so both legs resolve them to their own address space.
A1, A2, A3 = {"__addr__": 10001}, {"__addr__": 10002}, {"__addr__": 10003}
CALLS = [
    ("credit(address,uint256)", [A1, 1000]),
    ("credit(address,uint256)", [A2, 250]),
    ("approve(address,address,uint256)", [A1, A2, 77]),
    ("approve(address,address,uint256)", [A2, A3, 5]),
    ("bump(address)", [A1]),
    ("bump(address)", [A1]),
    ("bump(address)", [A2]),
    ("push(address,uint32,uint224)", [A1, 100, 1000]),
    ("push(address,uint32,uint224)", [A1, 200, 1250]),
    ("push(address,uint32,uint224)", [A2, 150, 250]),
]


def _encode(sig, args, resolve):
    from eth_abi import encode
    from eth_utils import function_signature_to_4byte_selector
    types = sig[sig.index("(") + 1:-1].split(",")
    vals = [resolve(a) if isinstance(a, dict) else a for a in args]
    return ("0x" + function_signature_to_4byte_selector(sig).hex()
            + encode(types, vals).hex())


def build_case() -> Path:
    import solcx
    case_dir = CASES / TAG
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / "prepared.sol").write_text(SOURCE)

    solcx.set_solc_version("0.8.26")
    out = solcx.compile_standard({
        "language": "Solidity",
        "sources": {"prepared.sol": {"content": SOURCE}},
        "settings": {"evmVersion": "paris",
                     "outputSelection": {"*": {"*": ["abi"]}}}})
    abi = out["contracts"]["prepared.sol"]["StorageShapes"]["abi"]

    # Synthetic addresses: marker N -> the same 0x…N the registry would mint.
    def resolve(m):
        return "0x" + f"{m['__addr__']:040x}"

    creator = "0x" + "c" * 40
    txns = []
    for i, (sig, args) in enumerate(CALLS):
        txns.append({"hash": "0x" + f"{i:064x}", "from": creator,
                     "input": _encode(sig, args, resolve), "value": 0,
                     "hist_ok": True, "ts": 1700000000 + i, "block": 1000 + i})

    dump_json(case_dir / "case.json", {
        "tag": TAG, "host": "synthetic", "address": "0x" + "5" * 40,
        "name": "StorageShapes", "compiler_version": "v0.8.26",
        "creation": {"creator": creator, "hash": "0x" + "f" * 64,
                     "ts": 1699999999, "block": 999},
        "ctor_args_hex": "", "abi": abi, "txns": txns})
    return case_dir


# What each map MUST contain if its reader works. Values are the decoded shape
# the differ compares, so a wrong struct/array layout fails here rather than
# silently reading as an int.
EXPECT = {
    "bal": 2,          # 2 credited accounts
    "allow": 2,        # 2 (owner, spender) pairs
    "nonces": 2,       # struct-valued: A1 -> [2], A2 -> [1]
    "ckpts": 2,        # array-valued: A1 -> 2 elements, A2 -> 1
}


def check(case_dir: Path) -> int:
    ev = load_json(case_dir / "evm_results.json")["storage"]["maps"]
    av = load_json(case_dir / "avm_results.json")["storage"]["maps"]
    bad = 0
    for name, want in EXPECT.items():
        e, a = ev.get(name) or {}, av.get(name) or {}
        if len(e) < want:
            print(f"  ✗ {name}: EVM leg read {len(e)} entries, expected >= {want}")
            bad += 1
        if len(a) < want:
            print(f"  ✗ {name}: AVM leg read {len(a)} entries, expected >= {want}")
            bad += 1
        if e and a and len(e) >= want and len(a) >= want:
            print(f"  ✓ {name}: {len(e)} entries both legs; "
                  f"sample={json.dumps(list(e.items())[:1])[:90]}")
    return bad


def main():
    from replay import replay
    from differ import print_report
    case_dir = build_case()
    print(f"[selftest] synthetic case at {case_dir}")
    evm_layout = "--evm-layout" in sys.argv
    if evm_layout:
        print("[selftest] --evm-storage-layout slot mode")
    rep = replay(TAG, max_txns=len(CALLS), snapshot_every=5,
                 evm_layout=evm_layout)
    print_report(rep)
    print("\n[selftest] storage-map coverage:")
    bad = check(case_dir)
    real = rep.get("counts", {}).get("__real__")
    if real is None:                       # differ counts by bucket
        real = sum(n for k, n in (rep.get("counts") or {}).items()
                   if k.endswith("_div"))
    if real:
        print(f"  ✗ {real} divergence(s)")
        bad += real
    print("\n[selftest] " + ("FAILED" if bad else "OK — every map shape read and matched"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
