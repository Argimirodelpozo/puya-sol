#!/usr/bin/env python3
"""Overnight randomized differential campaign — all generative axes, sequential.

Phases (each independent; a failure in one doesn't stop the rest):
  A  fuzz_gen plain expression trees        (broad operator coverage)
  B  fuzz_gen --cast                        (round-trip casts inside expressions)
  C  fuzz_gen --cf                          (control-flow bodies: loops/if/break)
  D  fuzz_gen --arr                         (array params, arr[i]/mat[i][j] in loops)
  E  fuzz_gen --bytes                       (bytesN bitwise/shift trees)
  F  fuzz_gen --cf --arr --cast --depth 5   (rich combined sweep)
  G  gen_stateful_contract -> fuzz_state.py (storage codec under sequences)
  H  gen_struct_contract   -> fuzz_state.py (packed struct-field storage codec)
  I  fuzz_crosscall.py fresh seeds          (cross-contract surfaces)

Everything is sequential ON PURPOSE (generated contracts share the name `G`;
concurrent compiles of same-named contracts are the known cache-poisoning trap).
Each phase's full output goes to run_campaign.log next to this file; stdout
carries one verdict line per phase. Exit code: number of phases with findings.
"""
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / "run_campaign.log"
PY = sys.executable

# (name, argv, timeout_s)
GEN = str(HERE / "fuzz_gen.py")
PHASES = [
    ("A-expr",   [PY, GEN, "--seed", "21000", "--contracts", "12", "--funcs", "24"], 3000),
    ("B-cast",   [PY, GEN, "--seed", "22000", "--contracts", "10", "--funcs", "20", "--cast"], 3000),
    ("C-cf",     [PY, GEN, "--seed", "23000", "--contracts", "10", "--cf"], 3000),
    ("D-arr",    [PY, GEN, "--seed", "24000", "--contracts", "10", "--arr"], 3000),
    ("E-bytes",  [PY, GEN, "--seed", "25000", "--contracts", "10", "--funcs", "20", "--bytes"], 3000),
    ("F-rich",   [PY, GEN, "--seed", "26000", "--contracts", "10", "--cf", "--arr", "--cast",
                  "--depth", "5"], 3600),
    ("I-cross1", [PY, str(HERE / "fuzz_crosscall.py"), "--fixtures", "12", "--seed", "42",
                  "--max-per-fn", "20"], 3000),
    ("I-cross2", [PY, str(HERE / "fuzz_crosscall.py"), "--fixtures", "12", "--seed", "43",
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
        out = ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")) \
            + ((e.stderr or b"").decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or ""))
        status = "TIMEOUT"
    except Exception as e:  # noqa: BLE001
        out, status = repr(e), "ERROR"
    dt = time.time() - t0
    log.write(f"\n{'='*80}\n### PHASE {name} ({dt:.0f}s) -> {status}\n{'='*80}\n{out}\n")
    log.flush()
    # extract a one-line summary (the differs print a final summary line)
    tail = [l for l in out.strip().splitlines() if l.strip()][-1] if out.strip() else ""
    return status, tail[:160]


def _shift_seed(argv: list[str], base: int) -> list[str]:
    """Return argv with the value after each `--seed` flag offset by `base`,
    so successive overnight cycles explore fresh programs."""
    if not base:
        return argv
    out = list(argv)
    for i, tok in enumerate(out):
        if tok == "--seed" and i + 1 < len(out):
            out[i + 1] = str(int(out[i + 1]) + base)
    return out


def main() -> None:
    # Optional seed offset: `fuzz_campaign.py <seed_base>` shifts every phase's
    # seed so an overnight loop doesn't regenerate identical programs each cycle.
    seed_base = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    findings = 0
    with LOG.open("w") as log:
        log.write(f"campaign start {time.strftime('%F %T')} seed_base={seed_base}\n")

        for name, argv, tmo in PHASES:
            status, tail = run_phase(name, _shift_seed(argv, seed_base), tmo, log)
            print(f"[{name:9s}] {status}: {tail}", flush=True)
            if status != "clean":
                findings += 1

        # G/H: generated stateful fixtures through fuzz_state.py
        sys.path.insert(0, str(HERE))
        import fuzz_gen  # noqa: E402

        for name, gen, seed0, count in (
            ("G-state", fuzz_gen.gen_stateful_contract, 27000 + seed_base, 14),
            ("H-struct", fuzz_gen.gen_struct_contract, 28000 + seed_base, 14),
        ):
            bad = 0
            for seed in range(seed0, seed0 + count):
                src = gen(seed)
                fix = HERE / "contracts" / f"_cg_{seed}.sol"
                fix.write_text(src)
                status, tail = run_phase(
                    f"{name}:{seed}",
                    [PY, str(HERE / "fuzz_state.py"), str(fix), "--max-per-fn", "16"],
                    900, log)
                if status != "clean":
                    bad += 1
                    print(f"[{name:9s}] seed {seed} {status}: {tail}", flush=True)
            print(f"[{name:9s}] {count - bad}/{count} clean", flush=True)
            if bad:
                findings += 1

        log.write(f"\ncampaign end {time.strftime('%F %T')}, phases with findings: {findings}\n")
    print(f"\n=== campaign done, phases with findings: {findings} (full log: {LOG}) ===")
    sys.exit(min(findings, 120))


if __name__ == "__main__":
    main()
