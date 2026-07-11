#!/usr/bin/env python3
"""Triage a campaign log: split KNOWN (already-documented) findings from NOVEL ones.

Overnight the generator keeps re-discovering the documented open DCE-drop bug
(test_dce_reverting_subexpr_literal_folds / puyabug #9): a `/` or `%` whose
zero-divisor revert is dropped because a LITERAL fold (shift>=256, *0, **, &0,
identical-branch ternary) makes its result unused. Those are not actionable
(fork-only fix, ruled out by policy). This classifier buckets each finding so a
human (or the overnight loop) only looks at what's new.

  python classify_findings.py run_campaign.log [more.log ...]
"""
import re
import sys
from pathlib import Path

# A finding block prints a body line and an `evm=… avm=…` line.
EVM_AVM = re.compile(r"evm=(\S+)\s+avm=(\S+)")
FOLD_CONSUMER = re.compile(
    r"(?:<<|>>)\s*(25[6-9]|2[6-9]\d|[3-9]\d\d|\d{4,})"   # shift EITHER way >= 256 folds to 0
    r"|\*\s*0\b|\*\*|&\s*0\b|\|\s*0\b", re.X)
DIVMOD = re.compile(r"[^/]/[^/]|%")   # a `/` (not `//`) or `%`


# Documented, fork-blocked puya BACKEND crash (puyabug.md #9): a cross-function
# constant-fold overflows a bit index. Any-N variant is the same class.
KNOWN_CRASH = re.compile(r"'n' must be <= 255")


def is_known_dce(body: str, evm: str, avm: str) -> bool:
    # Signature: EVM reverts, AVM returns a value, body has div/mod under a fold.
    if evm != "REVERT" or avm == "REVERT":
        return False
    return bool(DIVMOD.search(body)) and bool(FOLD_CONSUMER.search(body))


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit("usage: classify_findings.py <log> [<log> ...]")
    known = size_skip = crash = 0
    novel_blocks = []
    suppress_blocks = []
    crash_blocks = []
    for path in sys.argv[1:]:
        lines = Path(path).read_text(errors="replace").splitlines()
        last_body = ""
        for i, ln in enumerate(lines):
            if ln.strip().startswith("body:"):
                last_body = ln
                continue
            # Crash / deploy finding: `❌ seed N <contract>: <reason>`
            mc = re.search(r"<contract>:\s*(.*)", ln)
            if mc:
                reason = mc.group(1)
                if "create txn failed" in reason:      # oversized program (>8KB) — expected
                    size_skip += 1
                elif KNOWN_CRASH.search(reason):        # documented fold crash (puyabug #9)
                    known += 1
                else:                                   # backend crash — reproduce to classify
                    crash += 1
                    crash_blocks.append(ln.strip()[:200])
                continue
            # Value divergence: `… evm=X avm=Y`. The generating body is the nearest
            # preceding `body:` line (or embedded on the same finding record).
            m = EVM_AVM.search(ln)
            if not m:
                continue
            evm, avm = m.group(1), m.group(2)
            ctx = last_body + " " + ln
            rec = (last_body.strip() + "\n  " + ln.strip())[:600]
            if is_known_dce(ctx, evm, avm):
                known += 1
            elif evm == "REVERT" and avm != "REVERT":
                # EVM reverts, AVM returns a value. Empirically always the DCE-drop
                # class, but the fold isn't always visible (CF bodies) — surface as
                # LOW priority to spot-check, not as a hard novel bug.
                suppress_blocks.append(rec)
            else:
                # avm=REVERT where evm=value (the bool-tuple bug shape!), or a plain
                # value mismatch — the dangerous cases. HIGH priority.
                novel_blocks.append(rec)

    print(f"=== {known} known-DCE, {size_skip} oversized-skip, {crash} crash(repro), "
          f"{len(suppress_blocks)} revert-suppress(verify), {len(novel_blocks)} NOVEL ===\n")
    for i, b in enumerate(crash_blocks, 1):
        print(f"--- CRASH #{i} (reproduce to classify) " + "-" * 20 + f"\n{b}\n")
    for i, b in enumerate(novel_blocks, 1):
        print(f"--- NOVEL #{i} (HIGH: AVM reverts / value mismatch) " + "-" * 12 + f"\n{b}\n")
    for i, b in enumerate(suppress_blocks, 1):
        print(f"--- revert-suppress #{i} (likely DCE, verify body) " + "-" * 10 + f"\n{b}\n")
    sys.exit(1 if novel_blocks else 0)


if __name__ == "__main__":
    main()
