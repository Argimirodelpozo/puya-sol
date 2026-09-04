#!/usr/bin/env python3
"""Build the CCTP v2 out_avm artifacts (slot mode) with the two v2 patches.

  python3 build_v2_avm.py [cases-dir]

Patches (applied to a DISPOSABLE tree copy; the fetched corpus is immutable,
each application is printed and must hit or the build aborts):
  P1 Denylistable.notDenylistedCallers: the `msg.sender != tx.origin` branch
     is deleted — tx.origin has no distinct AVM meaning; origin == sender for
     every replayed root call (also true on the spoofed EVM leg), so the
     branch is unreachable and sender-only denylisting is exact.
  P2 `_disableInitializers();` in implementation constructors is neutralized —
     the replay deploys implementations DIRECTLY (no proxy hop) and then
     replays the proxy's historical initialize() calldata, which the
     UUPS-safety latch would otherwise reject.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent.parent
PUYA_SOL = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"

TAGS = {
    "cctp2_transmitter": "MessageTransmitterV2",
    "cctp2_messenger": "TokenMessengerV2",
    "cctp2_minter": "TokenMinterV2",
}

# v2's message parsing is heavier than v1's and a 16-txn group's pooled
# budget (16 x 700) is the protocol ceiling, so the hot entry points request
# their own budget via OpUp — what a real deployment does.
ENSURE_BUDGET = {
    "cctp2_transmitter": ["receiveMessage:45000", "sendMessage:20000"],
    "cctp2_messenger": [
        "handleReceiveMessage:30000",
        "depositForBurn:20000",
        "depositForBurnWithHook:20000",
    ],
    "cctp2_minter": ["mint:20000", "burn:20000"],
}

P1_MARK = "if (msg.sender != tx.origin) {"
P2_MARK = "_disableInitializers();"


def apply_patches(tree: Path, tag: str) -> list[str]:
    applied = []
    for path in sorted(tree.rglob("*.sol")):
        text = path.read_text()
        changed = text
        if P1_MARK in changed:
            lines = changed.splitlines(keepends=True)
            out, i = [], 0
            while i < len(lines):
                if P1_MARK in lines[i]:
                    depth = lines[i].count("{") - lines[i].count("}")
                    out.append(
                        "        // v2 replay patch P1: tx.origin == msg.sender on "
                        "AVM; origin-differs branch unreachable\n"
                    )
                    i += 1
                    while i < len(lines) and depth > 0:
                        depth += lines[i].count("{") - lines[i].count("}")
                        i += 1
                    applied.append(f"P1 tx.origin branch removed: {path.name}")
                    continue
                out.append(lines[i])
                i += 1
            changed = "".join(out)
        if P2_MARK in changed:
            changed = changed.replace(
                P2_MARK,
                "// v2 replay patch P2: initializers stay enabled for the "
                "direct-deploy + historical-initialize replay",
            )
            applied.append(f"P2 _disableInitializers neutralized: {path.name}")
        if changed != text:
            path.write_text(changed)
    return applied


def build(cases: Path, tag: str, contract: str) -> None:
    case_dir = cases / tag
    mf = json.loads((case_dir / "case.json").read_text())["multifile"]
    with tempfile.TemporaryDirectory(prefix=f"v2avm-{tag}-") as tmp:
        tree = Path(tmp) / "src"
        shutil.copytree(case_dir / "src", tree)
        applied = apply_patches(tree, tag)
        for line in applied:
            print(f"[{tag}] {line}")
        if tag == "cctp2_messenger" and not any("P1" in a for a in applied):
            sys.exit(f"[{tag}] expected the tx.origin patch to apply")
        # TokenMinterV2 is deployed directly on mainnet (no proxy, no
        # Initializable latch) — P2 applies only to the proxied contracts.
        if tag != "cctp2_minter" and not any("P2" in a for a in applied):
            sys.exit(f"[{tag}] expected the _disableInitializers patch to apply")
        out_dir = case_dir / "out_avm"
        out_dir.mkdir(exist_ok=True)
        cmd = [
            str(PUYA_SOL),
            "--source", str(tree / mf["main"]),
            *[a for f in mf["files"] for a in ("--source", str(tree / f))],
            "--import-path", str(tree),
            *[a for r in mf["remappings"] for a in ("--remapping", r)],
            "--legacy-source-rewrite",
            "--evm-storage-layout",
            *[a for b in ENSURE_BUDGET.get(tag, []) for a in ("--ensure-budget", b)],
            "--puya-path", str(PUYA),
            "--output-dir", str(out_dir),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        if res.returncode != 0:
            print(res.stdout[-2000:])
            print(res.stderr[-2000:])
            sys.exit(f"[{tag}] compile failed")
        size = (out_dir / f"{contract}.approval.bin").stat().st_size
        print(f"[{tag}] {contract}: {size} bytes approval")


def main() -> int:
    cases = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "cases"
    for tag, contract in TAGS.items():
        build(cases, tag, contract)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
