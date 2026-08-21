#!/usr/bin/env python3
"""Measure main-program size deltas for SimpleSplitter candidates.

This is deliberately case-agnostic: candidates come from a split-config file,
and each candidate is compiled alone against the case's fetched source tree.
It is a diagnostic for choosing profitable helper boundaries, not a source or
contract-name allowlist.

  python3 measure_split.py cases/<tag> [split.json]
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))

from framework.compile import compile_sol


def _load(path: Path):
    return json.loads(path.read_text())


def _candidates(config: dict) -> list[str]:
    if isinstance(config.get("delegate"), list):
        return list(dict.fromkeys(
            name for name in config["delegate"] if isinstance(name, str)
        ))
    helpers = config.get("helpers")
    if isinstance(helpers, list):
        return list(dict.fromkeys(
            name
            for helper in helpers
            for name in helper.get("extract", [])
            if isinstance(name, str)
        ))
    return list(dict.fromkeys(
        name for name in config.get("extract", []) if isinstance(name, str)
    ))


def _compile(case_dir: Path, case: dict, split_path: Path, out_dir: Path):
    mf = case.get("multifile")
    extra_args = ["--evm-layout", "--split-config", str(split_path)]
    if mf:
        src_root = out_dir / "src"
        shutil.copytree(case_dir / "src", src_root)
        artifacts = compile_sol(
            src_root / mf["main"],
            out_dir / "out",
            extra_sources=[src_root / rel for rel in mf["files"]],
            extra_import_dir=src_root,
            extra_remappings=mf["remappings"],
            extra_args=extra_args,
            timeout=300,
        )
    else:
        artifacts = compile_sol(
            case_dir / "prepared.sol", out_dir / "out",
            extra_args=extra_args, timeout=300,
        )
    sizes = {
        name: (art["arc56"].parent / f"{name}.approval.bin").stat().st_size
        for name, art in artifacts.by_contract.items()
    }
    metadata_path = out_dir / "out" / "delegate_helpers.json"
    metadata = _load(metadata_path) if metadata_path.exists() else None
    return sizes[case["name"]], sizes, metadata


def main() -> None:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    case_dir = Path(sys.argv[1]).resolve()
    config_path = (Path(sys.argv[2]).resolve() if len(sys.argv) == 3
                   else case_dir / "split.json")
    case = _load(case_dir / "case.json")
    source_config = _load(config_path)
    candidates = _candidates(source_config)
    candidate_key = "delegate" if "delegate" in source_config else "extract"
    baseline_path = case_dir / "out_avm" / f'{case["name"]}.approval.bin'
    baseline = baseline_path.stat().st_size
    print(f"baseline\t{baseline}", flush=True)

    results = []
    with tempfile.TemporaryDirectory(prefix="chd_split_measure_") as tmp:
        root = Path(tmp)
        for i, name in enumerate(candidates):
            work = root / f"candidate-{i:04d}"
            work.mkdir()
            split_path = work / "split.json"
            split_path.write_text(json.dumps({candidate_key: [name]}))
            try:
                size, sizes, metadata = _compile(
                    case_dir, case, split_path, work)
                row = {"name": name, "size": size, "delta": baseline - size,
                       "artifacts": sizes}
                if metadata:
                    row["delegates"] = metadata.get("delegates") or []
            except Exception as exc:
                stderr = getattr(exc, "stderr", "") or ""
                stdout = getattr(exc, "stdout", "") or ""
                detail = "\n".join(
                    line for line in (stdout + "\n" + stderr).splitlines()
                    if line.strip()
                )[-8000:]
                row = {"name": name, "error": str(exc), "detail": detail}
            results.append(row)
            if "size" in row:
                print(f'{name}\t{row["size"]}\t{row["delta"]:+d}', flush=True)
            else:
                print(f'{name}\tERROR\t{row["error"]}', flush=True)

    print(json.dumps({"baseline": baseline, "results": results}, indent=2))


if __name__ == "__main__":
    main()
