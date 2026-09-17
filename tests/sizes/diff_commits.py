#!/usr/bin/env python3
"""Measure two compiler revisions on exactly the same current corpus/backend.

Historical builds use temporary detached worktrees, removed after copying the
binary, standard library and provenance manifest. Only two compiler snapshots
and eight small census records are retained. Raw TEAL is temporary and only
requested with --teal-diffs; the selected output directory receives the diffs.
The current build must have a matching CMake source/binary manifest.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import sizes

ROOT = sizes.ROOT
COMPILERS = ROOT / "build/sizes/compilers"
CMAKE_KEYS = ("CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
              "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER",
              "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "BOOST_ROOT", "Boost_ROOT")


def rev_info(rev: str) -> tuple[str, str]:
    sha = sizes.git("rev-parse", "--verify", f"{rev}^{{commit}}")
    return sha, sizes.git("log", "-1", "--format=%h %ci %s", sha)


def cmake_settings() -> tuple[str | None, list[str]]:
    generator, definitions = None, []
    for line in (ROOT / "build/CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            generator = line.split("=", 1)[1]
        if any(line.startswith(key + ":") for key in CMAKE_KEYS):
            value = line.split("=", 1)[1]
            if value and not value.endswith("NOTFOUND"):
                definitions.append("-D" + line)
    return generator, definitions


def prune_compilers(directory: Path, keep: int = 2) -> None:
    entries = [p for p in directory.iterdir() if not p.is_symlink()
               and re.fullmatch(r"v1-[0-9a-f]{64}", p.name) and (p / ".sizes-owned").is_file()]
    for old in sorted(entries, key=lambda p: p.stat().st_mtime, reverse=True)[keep:]:
        print(f"removing size-tool compiler snapshot: {old}", file=sys.stderr)
        shutil.rmtree(old)


def build_commit(sha: str, jobs: int) -> Path:
    sizes.ensure_idle()
    generator, definitions = cmake_settings()
    key = sizes.digest({"revision": sha, "generator": generator, "definitions": definitions})
    directory = COMPILERS / ("v1-" + key)
    binary = directory / "puya-sol"
    if binary.exists():
        identity = sizes.compiler_identity(binary, None)
        if identity["root_commit"] != sha:
            raise ValueError(f"wrong revision in {directory}")
        directory.touch()
        return binary
    COMPILERS.mkdir(parents=True, exist_ok=True)
    interrupted = list((ROOT / "build/sizes").glob("size-build-*"))
    if interrupted:
        raise ValueError(f"interrupted size build needs recovery before allocating another: {interrupted[0]}")
    # Only this invocation's newly-created worktree may be removed below.
    scratch = Path(tempfile.mkdtemp(prefix="size-build-", dir=ROOT / "build/sizes"))
    wt = scratch / "checkout"
    added = False
    try:
        sizes.git("worktree", "add", "--detach", str(wt), sha)
        added = True
        subprocess.run(["git", "submodule", "update", "--init", "--recursive", "--depth", "1",
                        "--reference", str(ROOT / "solidity"), "solidity"], cwd=wt, check=True)
        before = sizes.source_identity(wt)
        configure = ["cmake", "-S", str(wt), "-B", str(wt / "build"),
                     "-DBUILD_TESTING=OFF", *definitions]
        if generator:
            configure += ["-G", generator]
        build = ["cmake", "--build", str(wt / "build"), "--parallel", str(jobs),
                 "--target", "puya-sol-build-manifest"]
        log = ROOT / "build/sizes/last-build.log"
        env = dict(os.environ, CCACHE_BASEDIR=str(wt), CCACHE_NOHASHDIR="1")
        print(f"building {sha[:12]}; log: {log}", file=sys.stderr, flush=True)
        with log.open("w") as stream:
            for command in (configure, build):
                subprocess.run(command, cwd=wt, env=env, stdout=stream,
                               stderr=subprocess.STDOUT, check=True)
        if sizes.source_identity(wt) != before:
            raise ValueError("historical build inputs changed during the build")
        # Old commits predate the source-hash manifest field. This tool just
        # completed their build in the verified detached checkout.
        manifest = wt / "build/puya-sol-build-manifest.txt"
        text = manifest.read_text()
        if "\nsource_sha256=" not in text:
            manifest.write_text(text + f"\nsource_sha256={before}\n")
        identity = sizes.compiler_identity(wt / "build/puya-sol", wt)
        if identity["root_commit"] != sha:
            raise ValueError("historical build manifest names the wrong revision")
        directory.mkdir()
        (directory / ".sizes-owned").touch()
        shutil.copy2(wt / "build/puya-sol", binary)
        shutil.copy2(manifest, directory / manifest.name)
        shutil.copytree(wt / "build/share", directory / "share")
    finally:
        if added:
            sizes.git("worktree", "remove", "--force", str(wt))
        shutil.rmtree(scratch)
    directory.touch()
    prune_compilers(COMPILERS)
    return binary


def write_teal_diffs(delta: dict, before: Path, after: Path, destination: Path) -> int:
    count = 0
    for change in delta["changed"]:
        rel = Path(change["source"]) / (change["contract"] + ".approval.teal")
        old, new = before / rel, after / rel
        if not (old.exists() and new.exists()):
            raise ValueError(f"missing requested TEAL for {rel}")
        target = destination / rel.with_suffix(".diff")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("".join(difflib.unified_diff(
            old.read_text().splitlines(keepends=True), new.read_text().splitlines(keepends=True),
            fromfile=f"base/{rel}", tofile=f"head/{rel}")))
        count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD~1")
    parser.add_argument("--head", help="historical revision; default: the verified current build")
    parser.add_argument("--base-compiler", type=Path)
    parser.add_argument("--head-compiler", type=Path)
    parser.add_argument("--build-jobs", type=sizes.positive, default=2)
    parser.add_argument("--teal-diffs", type=Path)
    parser.add_argument("--json", type=Path)
    sizes.add_common_args(parser)
    args = parser.parse_args()
    if args.puya is None:
        parser.error("Puya not found; supply --puya")
    with sizes.run_lock():
        base_sha, base_desc = rev_info(args.base)
        head_sha, head_desc = rev_info(args.head or "HEAD")
        base = args.base_compiler.resolve() if args.base_compiler else build_commit(base_sha, args.build_jobs)
        base_info = sizes.compiler_identity(base, None)
        if base_info["root_commit"] != base_sha:
            raise ValueError("--base-compiler does not match --base")
        if args.head_compiler:
            head = args.head_compiler.resolve()
        elif args.head:
            head = build_commit(head_sha, args.build_jobs)
        else:
            head = sizes.default_compiler()
        head_root = None if args.head else ROOT
        head_info = sizes.compiler_identity(head, head_root)
        if args.head and head_info["root_commit"] != head_sha:
            raise ValueError("--head-compiler does not match --head")
        head_label = "historical build" if args.head else "current build, including uncommitted source changes"
        print(f"base: {base_desc}\nhead: {head_desc} ({head_label})\n"
              "comparison: current corpus, same selected backend", flush=True)
        common = dict(jobs=args.jobs, timeout=args.timeout, only=args.only,
                      cache_dir=None if args.no_cache else sizes.DEFAULT_CACHE_DIR)
        with tempfile.TemporaryDirectory(prefix="size-teal-") as directory:
            old_teal, new_teal = Path(directory) / "base", Path(directory) / "head"
            old_rows, old_metadata = sizes.census(
                args.corpus, base, args.puya.resolve(), compiler_root=None,
                log_dir=args.log_dir and args.log_dir / "base",
                teal_dir=old_teal if args.teal_diffs else None, **common)
            new_rows, new_metadata = sizes.census(
                args.corpus, head, args.puya.resolve(), compiler_root=head_root,
                log_dir=args.log_dir and args.log_dir / "head",
                teal_dir=new_teal if args.teal_diffs else None, **common)
            if any(old_metadata[k] != new_metadata[k] for k in ("inputs", "backend")):
                raise ValueError("corpus or backend changed between the two measurements")
            delta = sizes.diff_tables({(r.source, r.name): r for r in old_rows},
                                      {(r.source, r.name): r for r in new_rows})
            sizes.print_diff(delta, args.limit)
            delta.update(base=old_metadata, head=new_metadata)
            if args.teal_diffs:
                print(f"TEAL diffs: {write_teal_diffs(delta, old_teal, new_teal, args.teal_diffs)}")
            if args.json:
                args.json.parent.mkdir(parents=True, exist_ok=True)
                args.json.write_text(json.dumps(delta, indent=2) + "\n")
        incomplete = any(r.approval in {"TIMEOUT", "ARTIFACT_ERR"} for r in old_rows + new_rows)
        return int(bool(delta["regressions"] or delta["removed"] or incomplete))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
