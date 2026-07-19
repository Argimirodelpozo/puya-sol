#!/usr/bin/env python3
"""Night-3 differential campaign — COLD-DIR corpus mutation + fresh generative seeds.

Night-2 declared the corpus saturated, but fuzz_mutate only ever mutated the
arithmetic-family fixture dirs (arithmetics/integer/conversions/...). The
semantic-test corpus has far larger, entirely UNMUTATED categories — and per the
coverage map the coldest builder handlers sit at the asm/memory seam. This
campaign points corpus-mutation at those cold dirs, and adds fresh generative
seeds (night-2 burned 21000-26000).

Also covers the NEW blob-backed bytes/string asm-pointer path (seam_strbuf.sol)
landed with the memory-pointer seam fix — code the fuzzer has never explored.

Sequential ON PURPOSE: generated contracts share the name `G`, and concurrent
compiles of same-named contracts race the .compile_cache (known poisoning trap).
Full output -> run_campaign_night3.log; stdout carries one verdict line per phase.
Exit code: number of phases with findings.
"""
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / "run_campaign_night3.log"
PY = sys.executable

GEN = str(HERE / "fuzz_gen.py")
MUT = str(HERE / "fuzz_mutate.py")
NEST = str(HERE / "fuzz_nested.py")
CROSS = str(HERE / "fuzz_crosscall.py")
STATE = str(HERE / "fuzz_state.py")

# (name, argv, timeout_s) — cold-dir mutation first: highest expected yield.
PHASES = [
    # ── COLD-DIR corpus mutation (never mutated before) ───────────────────────
    ("M-asm",    [PY, MUT, "--dirs", "inlineAssembly,memoryManagement,strings",
                  "--seed", "31000", "--fixtures", "60", "--mutants", "6"], 5400),
    ("M-agg",    [PY, MUT, "--dirs", "array,structs,types",
                  "--seed", "32000", "--fixtures", "60", "--mutants", "6"], 5400),
    ("M-store",  [PY, MUT, "--dirs", "storage,abiEncoderV2,abiEncodeDecode",
                  "--seed", "33000", "--fixtures", "60", "--mutants", "6"], 5400),
    ("M-call",   [PY, MUT, "--dirs", "functionCall,viaYul,libraries",
                  "--seed", "34000", "--fixtures", "60", "--mutants", "6"], 5400),
    ("M-ctrl",   [PY, MUT, "--dirs", "modifiers,statements,scoping,inheritance",
                  "--seed", "35000", "--fixtures", "50", "--mutants", "6"], 5400),
    ("M-enc",    [PY, MUT, "--dirs", "events,errors,reverts,getters",
                  "--seed", "36000", "--fixtures", "50", "--mutants", "6"], 5400),

    # ── NEW blob-backed bytes/string asm-pointer path ────────────────────────
    ("S-seam",   [PY, STATE, str(HERE / "contracts" / "seam_strbuf.sol"),
                  "--max-per-fn", "24"], 2400),

    # ── fresh generative seeds (night-2 burned 21000-26000) ──────────────────
    ("A-expr",   [PY, GEN, "--seed", "41000", "--contracts", "12", "--funcs", "24"], 3000),
    ("C-cf",     [PY, GEN, "--seed", "43000", "--contracts", "10", "--cf"], 3000),
    ("D-arr",    [PY, GEN, "--seed", "44000", "--contracts", "10", "--arr"], 3000),
    ("F-rich",   [PY, GEN, "--seed", "46000", "--contracts", "10", "--cf", "--arr",
                  "--cast", "--depth", "5"], 3600),
    ("N-nested", [PY, NEST, "--fixtures", "14", "--seed", "47000", "--max-per-fn", "16"], 3000),
    ("X-cross",  [PY, CROSS, "--fixtures", "12", "--seed", "48000",
                  "--max-per-fn", "20"], 3000),
]

OK_MARKERS = ("no divergences", "0 diverged", "0/")


def run_phase(name: str, argv: list[str], timeout: int, log) -> tuple[str, str]:
    t0 = time.time()
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        out = r.stdout + r.stderr
        status = "clean" if r.returncode == 0 else "FINDINGS"
    except subprocess.TimeoutExpired as e:
        def _dec(v):
            if isinstance(v, bytes):
                return v.decode(errors="replace")
            return v or ""
        out = _dec(e.stdout) + _dec(e.stderr)
        status = "TIMEOUT"
    except Exception as e:  # noqa: BLE001
        out, status = repr(e), "ERROR"
    dt = time.time() - t0
    log.write(f"\n{'=' * 72}\n=== PHASE {name}  [{status}]  {dt:.0f}s\n"
              f"=== argv: {' '.join(argv)}\n{'=' * 72}\n{out}\n")
    log.flush()
    tail = [l for l in out.splitlines() if l.strip()][-1:] or [""]
    return status, tail[0][:140]


def main():
    findings = []
    with open(LOG, "w") as log:
        log.write(f"night-3 campaign start\n")
        for name, argv, timeout in PHASES:
            print(f"[{time.strftime('%H:%M:%S')}] {name} …", flush=True)
            status, tail = run_phase(name, argv, timeout, log)
            mark = {"clean": "✅", "FINDINGS": "❌", "TIMEOUT": "⏱", "ERROR": "⚠️"}.get(status, "?")
            print(f"[{time.strftime('%H:%M:%S')}] {mark} {name}: {status} — {tail}", flush=True)
            if status != "clean":
                findings.append(name)
    print("\n" + "=" * 72)
    print(f"NIGHT-3 phases with findings: {len(findings)}"
          + (f" -> {', '.join(findings)}" if findings else " (all clean)"))
    print(f"full log: {LOG}")
    return len(findings)


if __name__ == "__main__":
    sys.exit(main())
