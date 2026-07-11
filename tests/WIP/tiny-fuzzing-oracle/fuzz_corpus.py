#!/usr/bin/env python3
"""CORPUS-HARVEST differential feed — run external Solidity fuzzing seeds through the differ.

Our generators (fuzz_gen/fuzz_nested/fuzz_crosscall) aim at the AVM-specific risk surface.
This complements them with BREADTH: solc's own oss-fuzz seed corpus
(github.com/ethereum/solidity-fuzzing-corpus, solc_ossfuzz_seed_corpus/ = ~15k raw .sol
files hand+machine authored to stress the compiler's grammar coverage — inheritance shapes,
loops, calldata forms, aggregate zoos we don't attempt).

The catch (why we can't just run all 15k): most corpus files include by-DESIGN divergence
constructs (gas, blockhash, dirty inline assembly, EVM memory-layout tricks, selfdestruct,
create2) that our differ treats as noise, OR use features puya-sol hard-errors on. So this is a
FILTER + FEED: keep only files that (a) parse+compile on BOTH solc and puya-sol, (b) contain no
blacklisted divergence construct, (c) expose at least one fuzzable pure/view function; run each
survivor through fuzz_evm's differ. A compile-error on either side is a SKIP (expected — the
corpus is adversarial), not a finding; only an AVM-vs-EVM value divergence on a mutually-
compiling contract is a real result.

  python fuzz_corpus.py <corpus_dir> [--limit N] [--max-per-fn M] [--seed S]

Prints per-file status; exits 1 if any survivor diverged.
"""
import random
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PUYA_SOL = HERE.parent.parent.parent / "build" / "puya-sol"
PUYA = HERE.parent.parent.parent / "puya" / ".venv" / "bin" / "puya"

# Constructs with BY-DESIGN AVM/EVM divergence or puya-sol non-support — skip the file rather
# than file a false finding. (Deliberately broad; the goal is clean signal, not coverage.)
BLACKLIST = re.compile(
    r"\b(gasleft|blockhash|selfdestruct|blobhash|create2|delegatecall|callcode|"
    r"extcodecopy|extcodehash|codesize|codecopy|gasprice|coinbase|difficulty|prevrandao|"
    r"mstore8|calldatacopy|returndatacopy|mcopy|tload|tstore|gas\s*:|\.gas\b)\b"
    r"|assembly", re.I)

PRAGMA_08 = re.compile(r"pragma\s+solidity\s+[^;]*0\.8")


def compiles_solc(path: Path) -> bool:
    # Reuse the differ's oracle path via a cheap introspect; but first a fast solc syntax gate
    # through puya-sol's bundled solc is implicit in the puya-sol compile, so just check puya-sol
    # here and let fuzz_evm's oracle reject on the EVM side.
    return True


def compiles_puyasol(path: Path) -> bool:
    try:
        r = subprocess.run(
            [str(PUYA_SOL), "--source", str(path), "--output-dir", "/tmp/fc_probe_out",
             "--puya-path", "/bin/true"],
            capture_output=True, text=True, timeout=60)
        return r.returncode == 0
    except Exception:
        return False


def prefilter(path: Path) -> str | None:
    """Return a skip-reason, or None if the file passes the static gate."""
    try:
        text = path.read_text(errors="replace")
    except Exception:
        return "unreadable"
    if "contract" not in text:
        return "no-contract"
    if not PRAGMA_08.search(text):
        return "not-0.8"
    # SINGLE-ENTITY only: fuzz_evm doesn't pin --contract, so a file with
    # multiple contracts / a library / a free function makes the EVM oracle
    # and the AVM harness pick DIFFERENT entities → the called signature is
    # method-not-found on one side → spurious REVERT divergence (verified on
    # the solc corpus: library `Lib.add` + free-fn `renameMe` files). Restrict
    # to exactly one contract, no library, no top-level function.
    if len(re.findall(r"\bcontract\s+\w+", text)) != 1:
        return "multi-contract"
    if re.search(r"\blibrary\s+\w+", text):
        return "has-library"
    # Free function = `function` at COLUMN 0 (contract methods are indented).
    if re.search(r"^function\s+\w+", text, re.M):
        return "has-free-function"
    if BLACKLIST.search(text):
        return "blacklisted-construct"
    # needs at least one plausibly-pure/view function to fuzz
    if not re.search(r"\bfunction\b.*\b(pure|view)\b", text):
        return "no-pure-view-fn"
    if len(text) > 8000:
        return "too-large"
    return None


def main() -> None:
    argv = list(sys.argv[1:])
    limit, mpf, seed = 200, 8, 1
    for flag in ("--limit", "--max-per-fn", "--seed"):
        if flag in argv:
            i = argv.index(flag)
            val = int(argv[i + 1]); del argv[i:i + 2]
            if flag == "--limit": limit = val
            elif flag == "--max-per-fn": mpf = val
            else: seed = val
    if not argv:
        sys.exit("usage: fuzz_corpus.py <corpus_dir> [--limit N] [--max-per-fn M] [--seed S]")
    corpus = Path(argv[0])
    files = sorted(p for p in corpus.rglob("*") if p.is_file())
    random.Random(seed).shuffle(files)

    skips: dict[str, int] = {}
    compile_skips = 0
    fed = 0
    diverged = []
    checked = 0
    for path in files:
        if fed >= limit:
            break
        reason = prefilter(path)
        if reason:
            skips[reason] = skips.get(reason, 0) + 1
            continue
        checked += 1
        if not compiles_puyasol(path):
            compile_skips += 1
            continue
        # Feed through the differ (fuzz_evm's oracle also gates the solc side).
        r = subprocess.run(
            [sys.executable, str(HERE / "fuzz_evm.py"), str(path), "--max-per-fn", str(mpf)],
            capture_output=True, text=True, timeout=600)
        out = r.stdout + r.stderr
        fed += 1
        if "no fuzzable" in out or "introspect/compile failed" in out or "EVM introspect" in out:
            skips["no-fuzzable-or-evm-reject"] = skips.get("no-fuzzable-or-evm-reject", 0) + 1
            fed -= 1
            continue
        if r.returncode != 0 and "DIVERGENCE" in out:
            diverged.append((path.name, out))
            print(f"[{fed}] ❌ DIVERGENCE {path.name}", flush=True)
            for line in out.splitlines():
                if "evm=" in line and "avm=" in line:
                    print("    " + line.strip())
        else:
            tag = "clean" if "no divergences" in out else "ran"
            print(f"[{fed}] {tag} {path.name}", flush=True)

    print(f"\n=== corpus feed: {fed} fed, {checked-compile_skips-fed} pre-differ drops, "
          f"{compile_skips} puya-sol compile-skips, {len(diverged)} diverged ===")
    print("static skips:", {k: v for k, v in sorted(skips.items(), key=lambda kv: -kv[1])})
    sys.exit(1 if diverged else 0)


if __name__ == "__main__":
    main()
