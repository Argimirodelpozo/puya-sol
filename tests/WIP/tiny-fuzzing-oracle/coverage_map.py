#!/usr/bin/env python3
"""Coverage-guided steering — Phase 1: build the COLD-HANDLER map.

Runs the coverage-instrumented puya-sol frontend (build-cov, compiled with
--coverage) over a corpus of .sol fixtures, accumulating gcov counters, then
reports per-builder-source-file line coverage. Files/regions the corpus barely
touches are where an untested miscompile is most likely to hide — the fuzzer
should be steered to exercise them.

Frontend-only (`--no-puya`): we measure the C++ BUILDER handlers (where the
miscompiles live), not the Python puya backend.

Usage:
  python coverage_map.py [--dirs a,b,..] [--all] [--top N]
    --all   run every corpus fixture (default: the fuzz_mutate eligible set)
    --top N show the N coldest builder files (default 40)
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent         # tests/WIP/tiny-fuzzing-oracle
ROOT = HERE.parent.parent.parent               # puya-sol repo root
COV_BIN = ROOT / "build-cov" / "puya-sol"
COV_OBJDIR = ROOT / "build-cov" / "CMakeFiles" / "puya-sol.dir" / "src"
SRC = ROOT / "src"
CORPUS = ROOT / "tests" / "solidity-semantic-tests" / "tests"


def all_fixtures(dirs=None):
    fixtures = []
    cats = dirs if dirs else [d.name for d in CORPUS.iterdir() if d.is_dir()]
    for d in cats:
        cdir = CORPUS / d / "contracts"
        if cdir.is_dir():
            fixtures += sorted(cdir.glob("*.sol"))
    return fixtures


def run_corpus(fixtures, out_dir):
    """Compile each fixture frontend-only through the coverage binary; the
    process accumulates .gcda counters in the object tree."""
    ok = fail = 0
    for i, f in enumerate(fixtures):
        try:
            p = subprocess.run(
                [str(COV_BIN), "--source", str(f), "--no-puya",
                 "--output-dir", str(out_dir)],
                capture_output=True, text=True, timeout=60)
            ok += (p.returncode == 0)
            fail += (p.returncode != 0)
        except subprocess.TimeoutExpired:
            fail += 1
        if (i + 1) % 100 == 0:
            print(f"  … {i + 1}/{len(fixtures)} fixtures", flush=True)
    return ok, fail


_GCOV_LINE = re.compile(r"^\s*(#####|-|\d+):\s*\d+:")


def gcov_file(gcda: Path):
    """Run gcov on one .gcda, return (source_path, executable_lines, covered_lines)
    for the builder source it maps to, or None."""
    # gcov emits <name>.gcov in cwd; run it in a temp-ish spot
    r = subprocess.run(["gcov", "-b", "-o", str(gcda.parent), str(gcda)],
                       capture_output=True, text=True, cwd="/tmp")
    # the .gcov filename is derived from the source; find builder sources
    return r.stdout


def collect_coverage():
    """Parse all .gcda under the coverage object dir via gcov; return
    {src_relpath: (covered_lines, executable_lines)}."""
    gcdas = list((ROOT / "build-cov").rglob("*.gcda"))
    print(f"[coverage] {len(gcdas)} .gcda counter files", flush=True)
    cov = {}
    import tempfile
    for gcda in gcdas:
        with tempfile.TemporaryDirectory() as td:
            subprocess.run(["gcov", "-o", str(gcda.parent), str(gcda)],
                           capture_output=True, text=True, cwd=td)
            for gcov in Path(td).glob("*.gcov"):
                text = gcov.read_text(errors="replace")
                m = re.search(r"^\s*-:\s*0:Source:(.+)$", text, re.MULTILINE)
                if not m:
                    continue
                srcpath = m.group(1)
                if "/src/builder/" not in srcpath and "/src/" not in srcpath:
                    continue
                rel = srcpath.split("/src/")[-1]
                covered = execu = 0
                for ln in text.splitlines():
                    mm = _GCOV_LINE.match(ln)
                    if not mm:
                        continue
                    tok = mm.group(1)
                    if tok == "-":
                        continue
                    execu += 1
                    if tok != "#####":
                        covered += 1
                # accumulate (a src may appear across multiple gcda if templated)
                pc, pe = cov.get(rel, (0, 0))
                cov[rel] = (max(pc, covered), max(pe, execu))
    return cov


def main():
    argv = sys.argv[1:]
    dirs = None
    if "--dirs" in argv:
        dirs = argv[argv.index("--dirs") + 1].split(",")
    run_all = "--all" in argv
    top = int(argv[argv.index("--top") + 1]) if "--top" in argv else 40
    skip_run = "--report-only" in argv

    if not COV_BIN.exists():
        sys.exit(f"coverage binary not built: {COV_BIN}\n"
                 f"build it: cmake --build build-cov -j6")

    if not skip_run:
        import fuzz_mutate as M
        fixtures = all_fixtures(dirs) if (run_all or dirs) else M.eligible_fixtures(M.DEFAULT_DIRS)
        print(f"[coverage] running {len(fixtures)} fixtures through the instrumented frontend")
        out = HERE / "out_cov"
        out.mkdir(exist_ok=True)
        ok, fail = run_corpus(fixtures, out)
        print(f"[coverage] {ok} compiled, {fail} failed (feature gaps / bad fixtures)")

    cov = collect_coverage()
    rows = []
    for rel, (c, e) in cov.items():
        if e == 0:
            continue
        rows.append((c / e, c, e, rel))
    rows.sort()                                    # coldest (lowest %) first

    print(f"\n{'=' * 78}\nCOLDEST {top} BUILDER FILES (corpus barely exercises these):")
    print(f"{'cov%':>6} {'covered/exec':>14}  file")
    for pctl, c, e, rel in rows[:top]:
        print(f"{pctl * 100:5.1f}% {c:>6}/{e:<7}  {rel}")

    tot_c = sum(c for _, c, e, _ in rows)
    tot_e = sum(e for _, c, e, _ in rows)
    print(f"\noverall builder line coverage: {tot_c}/{tot_e} = {100 * tot_c / tot_e:.1f}%")


if __name__ == "__main__":
    main()
