#!/usr/bin/env python3
"""cases/<tag>/upgrades.json (fetch.py --source-from/--upgrades output) →
joint-config "upgrades" entries, with per-era AVM artifacts compiled.

  ../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python gen_upgrades.py <tag> [...]
      [--into joint_config_X.json] [--no-avm-build]

For each detected era this decodes the implementation's constructor args and
the upgradeToAndCall embedded init calldata into the marker form both legs
resolve, compiles the era's source with puya-sol --evm-layout into
upgrade_<i>/out_avm, writes upgrade_<i>/abi.json, and prints (or merges with
--into) the ready-to-run "upgrades" entries. An UNVERIFIED era aborts: the
replay cannot cross an upgrade whose source is unknown — narrow the window
instead (--max-txns).
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from eth_abi import decode

from gen_v2_config import PROXY_ADMIN, decode_call, mark

HERE = Path(__file__).parent
CASES = HERE / "cases"
ROOT = HERE.parent.parent
PUYA_SOL = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"


def ctor_markers(abi: list, ctor_hex: str) -> list:
    ctor = next((e for e in abi if e.get("type") == "constructor"), None)
    if not ctor or not ctor.get("inputs") or not ctor_hex:
        return []
    types = [i["type"] for i in ctor["inputs"]]
    if any(t.startswith("tuple") for t in types):
        sys.exit("[gen_upgrades] tuple-typed ctor args not supported yet")
    vals = decode(types, bytes.fromhex(ctor_hex))
    return [mark(t, v) for t, v in zip(types, vals)]


def init_from_blobs(tag: str, abi: list, blobs: list[str]) -> tuple:
    """(init_sig, init_args) from the harvested calls into the proxy."""
    for raw in blobs:
        blob = bytes.fromhex(raw.removeprefix("0x"))
        sel = blob[:4].hex()
        if sel == "4f1ef286":  # upgradeToAndCall(address,bytes)
            _impl, data = decode(["address", "bytes"], blob[4:])
            if not data:
                return None, None
            d = decode_call(abi, data)
            if d is None:
                print(f"[{tag}] embedded init calldata {data[:4].hex()} does "
                      f"not decode against the new ABI — entry has no init")
                return None, None
            sig, values, types = d
            print(f"[{tag}] init: {sig}")
            return sig, [mark(t, v) for t, v in zip(types, values)]
        if sel in PROXY_ADMIN:
            continue
        d = decode_call(abi, blob)
        if d is not None:
            sig, values, types = d
            print(f"[{tag}] init (sibling call): {sig}")
            return sig, [mark(t, v) for t, v in zip(types, values)]
    return None, None


def build_avm(tag: str, up: dict) -> None:
    src_dir = CASES / tag / up["dir"]
    out_dir = src_dir / "out_avm"
    out_dir.mkdir(exist_ok=True)
    mf = up.get("multifile")
    if mf:
        tree = src_dir / "src"
        cmd = [
            str(PUYA_SOL),
            "--source", str(tree / mf["main"]),
            *[a for f in mf["files"] for a in ("--source", str(tree / f))],
            "--import-path", str(tree),
            *[a for r in mf["remappings"] for a in ("--remapping", r)],
        ]
    else:
        cmd = [str(PUYA_SOL), "--source", str(src_dir / "prepared.sol")]
    cmd += ["--evm-layout", "--puya-path", str(PUYA),
            "--output-dir", str(out_dir)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    approval = out_dir / f"{up['name']}.approval.bin"
    if res.returncode != 0 or not approval.exists():
        print(res.stdout[-2000:])
        print(res.stderr[-2000:])
        sys.exit(f"[gen_upgrades] {tag}/{up['dir']}: AVM compile failed")
    print(f"[{tag}] {up['dir']}: {up['name']} "
          f"{approval.stat().st_size} bytes approval")


def entries_for(tag: str, avm_build: bool) -> list[dict]:
    meta = json.loads((CASES / tag / "upgrades.json").read_text())
    out = []
    for up in meta["upgrades"]:
        if up.get("unverified"):
            sys.exit(f"[gen_upgrades] {tag}/{up['dir']}: implementation "
                     f"{up['impl']} is unverified — the replay cannot cross "
                     f"this upgrade; narrow the window (--max-txns) instead")
        abi = up["abi"]
        (CASES / tag / up["dir"] / "abi.json").write_text(json.dumps(abi))
        if avm_build:
            build_avm(tag, up)
        init_sig, init_args = init_from_blobs(
            tag, abi, up.get("init_calldata") or [])
        out.append({
            "tag": tag,
            "block": int(up["block"]),
            "txindex": int(up.get("txindex") or 0),
            "ts": int(up["ts"]),
            "hash": up.get("hash"),
            "impl": up.get("impl"),
            "contract": up["name"],
            "avm_artifact": f"{tag}/{up['dir']}/out_avm",
            "abi": f"{tag}/{up['dir']}/abi.json",
            "src": (f"{tag}/{up['dir']}/src" if up.get("multifile")
                    else f"{tag}/{up['dir']}"),
            "multifile": up.get("multifile"),
            "ctor_args": ctor_markers(abi, up.get("ctor_args_hex") or ""),
            "init_sig": init_sig,
            "init_args": init_args,
            "sender": up.get("sender"),
        })
    return out


def main() -> int:
    argv = list(sys.argv[1:])
    into = None
    if "--into" in argv:
        i = argv.index("--into")
        into = Path(argv[i + 1])
        del argv[i:i + 2]
    avm_build = "--no-avm-build" not in argv
    if not avm_build:
        argv.remove("--no-avm-build")
    if not argv:
        sys.exit(__doc__)
    entries = []
    for tag in argv:
        entries.extend(entries_for(tag, avm_build))
    entries.sort(key=lambda e: (e["block"], e["txindex"]))
    if into:
        config = json.loads(into.read_text())
        kept = [e for e in (config.get("upgrades") or [])
                if e["tag"] not in set(argv)]
        config["upgrades"] = sorted(kept + entries,
                                    key=lambda e: (e["block"], e["txindex"]))
        into.write_text(json.dumps(config, indent=1))
        print(f"[gen_upgrades] {len(entries)} entrie(s) merged into {into}")
    else:
        print(json.dumps({"upgrades": entries}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
