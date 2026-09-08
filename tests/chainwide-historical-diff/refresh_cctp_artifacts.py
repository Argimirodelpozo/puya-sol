#!/usr/bin/env python3
"""Recompile the CCTP v1 joint-lane artifacts with the CURRENT compiler.

  python3 refresh_cctp_artifacts.py [tag ...] [--cases DIR]
                                    (default tags: cctp_transmitter cctp_messenger cctp_minter)

Writes cases/<tag>/out_avm_joint/<Contract>.{approval,clear}.{teal,bin} and
<Contract>.arc56.json through the semantic-test framework's compile path
(build/puya-sol → puya, `--legacy-source-rewrite` plus the research
allow-divergence list — the same path the per-contract lane used for the
certified 2026-08-19 artifacts), in the two modes the joint oracle lane
(oracle_cctp_historical.py) depends on:

  * Explicit --contract-abi arc4: the driver encodes every historical call
    against the ARC-56 method list. The separate out_avm_joint directory is
    never overwritten by a per-contract replay's EVM-ABI out_avm compilation.
  * --evm-storage-layout: the joint storage comparison is slot-for-slot against
    the EVM leg's traced SSTOREs; a named-cell artifact yields an empty slot map
    and a vacuous storage lane (the 2026-09-06 refresh did exactly that).

For cctp_minter the StubERC20 dependency (deps/argdep_<usdc>/prepared.sol) is
recompiled too: both joint configs deploy the stub from that out_avm_joint.

Compile only — nothing is deployed and LocalNet is not contacted.  Multi-file
(v2) cases are refused: build_v2_avm.py owns those (source patches P1/P2 and
per-method --ensure-budget).  Every artifact is validated afterwards with the
driver's own check (`oracle_cctp_historical.py --check-artifacts`), so a
compile that silently came out in the wrong profile or storage model fails
here, not in the middle of a replay.
"""

from __future__ import annotations

import argparse
import os
import runpy
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))

from chd_common import CASES, load_json  # noqa: E402
from oracle_cctp_historical import JOINT_ARTIFACT_DIR  # noqa: E402

DEFAULT_TAGS = ("cctp_transmitter", "cctp_messenger", "cctp_minter")
# Flags on top of the framework's defaults.  Never --contract-abi evm here.
JOINT_LANE_ARGS = ["--contract-abi", "arc4", "--evm-storage-layout"]


def compile_into(out_dir: Path, source: Path, label: str) -> list[str]:
    """Compile one single-file source into out_dir; returns contract names."""
    from framework import Harness

    # Harness only uses its LocalNet handle to deploy; compile never touches it.
    harness = Harness(None, out_dir)
    started = time.time()
    artifacts = harness.compile(source, extra_args=list(JOINT_LANE_ARGS))
    names = sorted(artifacts.by_contract)
    print(
        f"[refresh] {label}: compiled {names} in {time.time() - started:.0f}s "
        f"({' '.join(JOINT_LANE_ARGS)}) -> {out_dir}",
        flush=True,
    )
    return names


def refresh(tag: str, cases: Path, driver: dict) -> None:
    case_dir = cases / tag
    case = load_json(case_dir / "case.json")
    if case.get("multifile"):
        sys.exit(
            f"[refresh] {tag} is a multi-file (v2) case; build it with "
            f"`python3 build_v2_avm.py {cases}` instead"
        )
    out_dir = case_dir / JOINT_ARTIFACT_DIR
    stub_tag = driver["STUB_SOURCE"]["tag"]
    if tag == stub_tag:
        # The stub first, so the main contract's awst.json/options.json/log
        # are the ones left at the top level of out_avm_joint.
        stub_dir = case_dir / "deps" / f"argdep_{driver['STUB_CONFIG']['address'][2:10]}"
        stub_source = stub_dir / "prepared.sol"
        if not stub_source.exists():
            sys.exit(f"[refresh] {tag}: StubERC20 source missing at {stub_source}")
        compile_into(out_dir, stub_source, f"{tag} StubERC20 dependency")
    compile_into(out_dir, case_dir / "prepared.sol", tag)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tags", nargs="*", default=list(DEFAULT_TAGS))
    parser.add_argument("--cases", type=Path, default=CASES)
    args = parser.parse_args(argv)
    cases = args.cases.resolve()
    # MessageTransmitter is a 16 KB O2 program; the framework's 120 s default
    # is sized for the semantic suite.
    os.environ.setdefault("PUYA_SOL_COMPILE_TIMEOUT", "900")
    driver = runpy.run_path(str(HERE / "oracle_cctp_historical.py"))
    for tag in args.tags:
        refresh(tag, cases, driver)
    try:
        checked = driver["validate_joint_artifacts"](cases, tags=set(args.tags))
    except driver["ArtifactError"] as error:
        print(f"[refresh] artifact check FAILED: {error}", file=sys.stderr)
        return 1
    for label, info in checked.items():
        print(
            f"[refresh] artifact ok: {label}: {info['methods']} ARC-4 methods, "
            f"{info['signatures_checked']} signature(s) resolve, "
            f"{info['storage_model']}, {info['approval_bytes']} B approval"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
