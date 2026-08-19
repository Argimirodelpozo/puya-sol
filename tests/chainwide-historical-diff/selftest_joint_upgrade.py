#!/usr/bin/env python3
"""Self-test for MID-HISTORY UPGRADES in the joint replay (proxy.md §1).

No real corpus contract upgraded inside its harvested window, so the upgrade
plumbing would otherwise ship untested. This synthesizes a two-era case:

  V1  credit(a, v): bal[a] += v
  V2  credit(a, v): require(v >= 10), bal[a] += 2*v, emits CreditedV2

and a five-call history with an upgrade between calls 2 and 3, including one
post-upgrade call that only fails under V2 semantics (hist_ok=False). A leg
that misses the upgrade diverges in all three lanes at once: status (the V2
require), events (CreditedV2), and storage (the doubled credits).

  python3 selftest_joint_upgrade.py    # system python (solc via EVM venv legs)

Ground-truth logs are taken from the EVM leg's own replay (contracts are
re-homed at their historical addresses, so its log addresses are already the
"historical" form), which makes the differ's event lane an AVM-vs-EVM check.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
CASES = HERE / "cases"
ROOT = HERE.parent.parent
PUYA_SOL = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"
EVM_PY = ROOT / "tests" / "WIP" / "tiny-fuzzing-oracle" / ".evmvenv" / "bin" / "python"
PROVER = Path(
    os.environ.get(
        "AVM_PROVER_ROOT",
        Path.home()
        / "AlgorandFoundation/SideProjects/new_verifier_experiment/experiment_3",
    )
)

TAG = "upgtest"
ADDRESS = "0x7a61b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3"
CREATOR = "0x" + "cc" * 20
A1 = "0x" + "aa" * 20
A2 = "0x" + "ab" * 20
A3 = "0x" + "ac" * 20
SEED = 7

V1 = """// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract Counter {
    uint256 public total;
    mapping(address => uint256) public bal;
    event Credited(address who, uint256 amount, uint256 total);

    constructor(uint256 seed) { total = seed; }

    function credit(address a, uint256 v) external {
        bal[a] += v;
        total += v;
        emit Credited(a, v, total);
    }
}
"""

V2 = """// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract Counter {
    uint256 public total;
    mapping(address => uint256) public bal;
    event Credited(address who, uint256 amount, uint256 total);
    event CreditedV2(address who, uint256 amount, uint256 total);

    constructor(uint256 seed) { total = seed; }

    function credit(address a, uint256 v) external {
        require(v >= 10, "min credit");
        bal[a] += 2 * v;
        total += 2 * v;
        emit CreditedV2(a, v, total);
    }
}
"""

# (who, amount, hist_ok, block) — the upgrade lands at block 1005.
CALLS = [
    (A1, 100, True, 1001),
    (A2, 5, True, 1002),       # V1 accepts small credits
    (A1, 100, True, 1006),     # V2: +200
    (A3, 5, False, 1007),      # V2 min-credit require rejects
    (A3, 50, True, 1008),      # V2: +100
]
UPGRADE_BLOCK = 1005
FINAL_TOTAL = SEED + 100 + 5 + 200 + 100  # 412


def compile_avm(source_dir: Path, out_dir: Path) -> None:
    out_dir.mkdir(exist_ok=True)
    res = subprocess.run(
        [
            str(PUYA_SOL),
            "--source", str(source_dir / "prepared.sol"),
            "--evm-layout",
            "--puya-path", str(PUYA),
            "--output-dir", str(out_dir),
        ],
        capture_output=True, text=True, timeout=900,
    )
    if res.returncode != 0:
        print(res.stdout[-2000:])
        print(res.stderr[-2000:])
        sys.exit(f"[upgtest] AVM compile failed for {source_dir}")


def solc_abi(source: str) -> list:
    res = subprocess.run(
        [
            str(EVM_PY), "-c",
            "import sys, json, solcx;"
            "solcx.set_solc_version('0.8.26');"
            "out = solcx.compile_standard({'language': 'Solidity',"
            " 'sources': {'prepared.sol': {'content': sys.stdin.read()}},"
            " 'settings': {'outputSelection': {'*': {'*': ['abi']}}}});"
            "print(json.dumps(out['contracts']['prepared.sol']['Counter']['abi']))",
        ],
        input=source, capture_output=True, text=True, timeout=300,
    )
    if res.returncode != 0:
        sys.exit(f"[upgtest] solc abi failed: {res.stderr[-1500:]}")
    return json.loads(res.stdout)


def build_case() -> None:
    case_dir = CASES / TAG
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / "prepared.sol").write_text(V1)
    (case_dir / "src_v2").mkdir(exist_ok=True)
    (case_dir / "src_v2" / "prepared.sol").write_text(V2)

    abi_v1 = solc_abi(V1)
    (case_dir / "abi_v2.json").write_text(json.dumps(solc_abi(V2)))

    txns, calls = [], []
    for i, (who, amount, ok, block) in enumerate(CALLS):
        txns.append({
            "hash": "0x" + f"{i + 1:064x}", "from": CREATOR, "input": "0x",
            "value": 0, "hist_ok": ok, "ts": 1700000000 + block,
            "block": block, "txindex": 0,
        })
        calls.append({
            "i": i, "hash": txns[-1]["hash"], "ts": txns[-1]["ts"],
            "hist_ok": ok, "value": 0, "sender": None,
            "sig": "credit(address,uint256)",
            "args": [{"__dep__": who}, amount], "skip": None,
        })
    json.dump({
        "tag": TAG, "host": "synthetic", "address": ADDRESS, "name": "Counter",
        "compiler_version": "v0.8.26",
        "creation": {"creator": CREATOR, "hash": "0x" + "f" * 64,
                     "ts": 1700000000, "block": 999},
        "ctor_args_hex": "", "abi": abi_v1, "txns": txns,
    }, (case_dir / "case.json").open("w"), indent=1)
    json.dump(
        {"calls": calls, "meta": {"ctor_args": [SEED]}},
        (case_dir / "calls.json").open("w"), indent=1,
    )
    json.dump(
        {"creator": CREATOR, "senders": {}, "args": {}},
        (case_dir / "registry.json").open("w"), indent=1,
    )

    compile_avm(case_dir, case_dir / "out_avm")
    compile_avm(case_dir / "src_v2", case_dir / "out_avm_v2")

    json.dump({
        "cases": {TAG: {"contract": "Counter", "address": ADDRESS,
                        "app_id": int(ADDRESS[-16:], 16)}},
        "init_calls": [],
        "upgrades": [{
            "tag": TAG, "block": UPGRADE_BLOCK, "txindex": 0,
            "ts": 1700000000 + UPGRADE_BLOCK,
            "hash": "0x" + "e" * 64, "impl": "0x" + "d" * 40,
            "contract": "Counter",
            "avm_artifact": f"{TAG}/out_avm_v2",
            "abi": f"{TAG}/abi_v2.json",
            "src": f"{TAG}/src_v2",
            "ctor_args": [0],
        }],
    }, (HERE / "joint_config_upgtest.json").open("w"), indent=1)


def build_generator_inputs() -> None:
    """Synthesize what fetch.py would have written, so gen_upgrades.py runs
    on the same two-era case: upgrades.json + upgrade_0/prepared.sol, with an
    upgradeToAndCall blob whose embedded init calldata is EMPTY."""
    import shutil

    case_dir = CASES / TAG
    up_dir = case_dir / "upgrade_0"
    up_dir.mkdir(exist_ok=True)
    (up_dir / "prepared.sol").write_text(V2)
    shutil.rmtree(up_dir / "out_avm", ignore_errors=True)
    impl = "0x" + "d" * 40
    blob = ("0x4f1ef286"
            + "00" * 12 + impl[2:]                      # address impl
            + f"{0x40:064x}" + f"{0:064x}")             # bytes data = ""
    json.dump({
        "address": ADDRESS,
        "upgrades": [{
            "hash": "0x" + "e" * 64, "impl": impl,
            "block": UPGRADE_BLOCK, "txindex": 0,
            "ts": 1700000000 + UPGRADE_BLOCK,
            "dir": "upgrade_0", "sender": CREATOR,
            "init_calldata": [blob],
            "name": "Counter", "compiler_version": "v0.8.26",
            "abi": json.loads((case_dir / "abi_v2.json").read_text()),
            "ctor_args_hex": "00" * 32,                 # uint256 seed = 0
            "multifile": None,
        }],
    }, (case_dir / "upgrades.json").open("w"), indent=1)


def check_generated_entries(config_path: Path) -> int:
    got = json.loads(config_path.read_text())["upgrades"]
    bad = 0
    if len(got) != 1:
        print(f"[upgtest] ✗ generator emitted {len(got)} entries, wanted 1")
        return 1
    e = got[0]
    want = {"tag": TAG, "block": UPGRADE_BLOCK, "contract": "Counter",
            "avm_artifact": f"{TAG}/upgrade_0/out_avm",
            "abi": f"{TAG}/upgrade_0/abi.json", "src": f"{TAG}/upgrade_0",
            "ctor_args": [0], "init_sig": None}
    for k, v in want.items():
        if e.get(k) != v:
            print(f"[upgtest] ✗ generated entry {k}: {e.get(k)!r} != {v!r}")
            bad += 1
    if not (CASES / TAG / "upgrade_0" / "out_avm" / "Counter.approval.bin"
            ).exists():
        print("[upgtest] ✗ generator did not compile the era's AVM artifact")
        bad += 1
    return bad


def run(cmd: list[str], label: str) -> subprocess.CompletedProcess:
    print(f"[upgtest] {label}: {' '.join(str(c) for c in cmd[:3])} ...")
    res = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                         timeout=1800)
    sys.stdout.write(res.stdout[-1500:])
    if res.stderr:
        sys.stderr.write(res.stderr[-1500:])
    return res


def main() -> int:
    build_case()
    config = HERE / "joint_config_upgtest.json"
    evm_out = CASES / TAG / "evm_results.json"
    avm_out = CASES / TAG / "avm_report.json"

    res = run([EVM_PY, HERE / "cctp_evm_leg.py", CASES, "--config", config,
               "--output", evm_out], "EVM leg")
    if res.returncode != 0:
        sys.exit("[upgtest] EVM leg failed")
    evm = json.loads(evm_out.read_text())

    # Ground-truth logs for the differ's event lane: the EVM leg's own logs
    # (addresses are historical after re-homing).
    (CASES / TAG / "logs.json").write_text(json.dumps(evm["logs"]))

    res = run(["python3", HERE / "oracle_cctp_historical.py", CASES,
               "--prover-root", PROVER, "--config", config,
               "--continue-after-divergence", "--output", avm_out], "AVM leg")
    if res.returncode != 0:
        sys.exit("[upgtest] AVM leg reported status mismatches")
    avm = json.loads(avm_out.read_text())

    res = run(["python3", HERE / "cctp_joint_diff.py", CASES, "--avm", avm_out,
               "--evm", evm_out, "--config", config], "differ")

    bad = 0
    if res.returncode != 0:
        print("[upgtest] ✗ differ reported findings")
        bad += 1
    if avm["summary"]["status_mismatches"]:
        print("[upgtest] ✗ AVM status mismatches")
        bad += 1
    if len(avm["scope"]["mid_history_upgrades"]) != 1:
        print("[upgtest] ✗ upgrade did not apply on the AVM leg")
        bad += 1
    total_word = (avm.get("final_storage") or {}).get(TAG, {}).get("0")
    if total_word != f"{FINAL_TOTAL:064x}":
        print(f"[upgtest] ✗ AVM total slot: {total_word} != {FINAL_TOTAL}")
        bad += 1
    evm_total = (evm.get("storage") or {}).get(TAG, {}).get("0")
    if evm_total != f"{FINAL_TOTAL:064x}":
        print(f"[upgtest] ✗ EVM total slot: {evm_total} != {FINAL_TOTAL}")
        bad += 1
    if not any("code swap" in s["name"] for s in evm["steps"]):
        print("[upgtest] ✗ upgrade did not apply on the EVM leg")
        bad += 1

    # ── generator path: fetch-shaped upgrades.json → gen_upgrades.py →
    # entries + compiled artifacts → the SAME green replay.
    build_generator_inputs()
    gen_config = HERE / "joint_config_upgtest_gen.json"
    base = json.loads(config.read_text())
    base.pop("upgrades", None)
    gen_config.write_text(json.dumps(base, indent=1))
    res = run([EVM_PY, HERE / "gen_upgrades.py", TAG, "--into", gen_config],
              "generator")
    if res.returncode != 0:
        print("[upgtest] ✗ gen_upgrades.py failed")
        return 1
    bad += check_generated_entries(gen_config)
    res = run([EVM_PY, HERE / "cctp_evm_leg.py", CASES, "--config", gen_config,
               "--output", evm_out], "EVM leg (generated config)")
    bad += res.returncode != 0
    res = run(["python3", HERE / "oracle_cctp_historical.py", CASES,
               "--prover-root", PROVER, "--config", gen_config,
               "--continue-after-divergence", "--output", avm_out],
              "AVM leg (generated config)")
    bad += res.returncode != 0
    res = run(["python3", HERE / "cctp_joint_diff.py", CASES, "--avm", avm_out,
               "--evm", evm_out, "--config", gen_config],
              "differ (generated config)")
    if res.returncode != 0:
        print("[upgtest] ✗ differ findings under the GENERATED config")
        bad += 1

    print("\n[upgtest] " + ("FAILED" if bad else
                            "OK — upgrade applied on both legs, 0 findings "
                            "(hand + generated configs)"))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
