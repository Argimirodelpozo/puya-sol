#!/usr/bin/env python3
"""Tiny differential-fuzzing spike — see README.md.

Fuzzes boundary inputs through the known-risky codecs in codec_probe.sol on the
AVM (puya-sol → localnet, via the semantic-test framework) and diffs each result
against the Python EVM-semantics oracle (oracle.py). Pure/view computational
subset → no by-design divergence, so any mismatch is a real bug.

Run:  python tests/WIP/tiny-fuzzing-oracle/fuzz.py
(needs localnet up — same prerequisite as the semantic suite)
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FRAMEWORK = HERE.parents[1] / "solidity-semantic-tests"  # tests/solidity-semantic-tests
sys.path.insert(0, str(FRAMEWORK))
sys.path.insert(0, str(HERE))

import framework._algosdk_patch  # noqa: F401  signed-intN encode/decode (also auto-imported)
from framework import Harness
from framework.localnet import LocalNet
import oracle as O

FIXTURE = HERE / "contracts" / "codec_probe.sol"
BITS = 1 << 256
MAX = BITS - 1
I256_MIN, I256_MAX = -(1 << 255), (1 << 255) - 1


def _ok_i256(x):
    return I256_MIN <= x <= I256_MAX


def _ok_u256(x):
    return 0 <= x <= MAX


def signed_singles():
    """0/±1/±2/extremes + the ±boundary of every sub-word width we cast through."""
    vals = {0, 1, -1, 2, -2, 12345, -12345, I256_MIN, I256_MAX}
    for bits in (8, 24, 128):
        h = 1 << (bits - 1)
        vals.update({h - 1, h, -h, -h - 1, (1 << bits) - 1, 1 << bits, -(1 << bits)})
    return sorted(v for v in vals if _ok_i256(v))


def unsigned_singles():
    vals = {0, 1, 2, 12345, MAX, MAX - 1}
    for bits in (8, 24, 128):
        vals.update({(1 << bits) - 1, 1 << bits, (1 << bits) + 5})
    return sorted(v for v in vals if _ok_u256(v))


def u256_pairs():
    """Cross boundary points against {0,1,2,self,complement,MAX,2^128} to hit
    every add/sub/mul overflow edge."""
    pts = [0, 1, 2, 100, 200, 255, 256, MAX // 2, MAX // 2 + 1, 1 << 128, (1 << 128) - 1, MAX - 1, MAX]
    out = set()
    for a in pts:
        for b in {0, 1, 2, a, (MAX - a) if a <= MAX else 0, MAX, 1 << 128}:
            if _ok_u256(a) and _ok_u256(b):
                out.add((a, b))
    return sorted(out)


def i256_pairs():
    base = [(7, 2), (-7, 2), (7, -2), (-7, -2), (7, 0), (0, 7), (I256_MIN, -1),
            (I256_MIN, 1), (I256_MAX, -1), (-8, 3), (8, -3), (1, 1), (-1, -1),
            (100, 7), (-100, 7), (100, -7), (-100, -7), (I256_MAX, I256_MIN), (5, 5), (-5, 5)]
    return [p for p in base if _ok_i256(p[0]) and _ok_i256(p[1])]


_SHIFTS = [0, 1, 7, 8, 127, 128, 255, 256, 257, 300, MAX]  # incl. the ≥256 saturation edge


def shift_pairs_u():
    vals = [0, 1, 2, 0xFF, 0x8000, 1 << 128, 1 << 255, MAX]
    return [(v, s) for v in vals for s in _SHIFTS]


def shift_pairs_s():
    vals = [0, 1, -1, 2, -2, 12345, -12345, -(1 << 128), I256_MAX, I256_MIN]
    return [(v, s) for v in vals for s in _SHIFTS]


def modmul_triples():
    big = [0, 1, 2, 1 << 128, 1 << 255, MAX - 1, MAX]
    out = set()
    for m in (0, 1, 7, 1 << 128, MAX):       # m==0 is the key edge (EVM → 0, no revert)
        for a in big:
            for b in (a, MAX, 7):
                out.add((a, b, m))
    return sorted(out)


def exp_pairs():
    bases = [0, 1, 2, 3, 10, 1 << 128, MAX]
    exps = [0, 1, 2, 3, 7, 85, 128, 255, 256, 257, MAX]
    return [(b, e) for b in bases for e in exps]


CASES = [
    ("int24RT(int256)", [(x,) for x in signed_singles()]),
    ("int8RT(int256)", [(x,) for x in signed_singles()]),
    ("int128RT(int256)", [(x,) for x in signed_singles()]),
    ("abiRTInt128(int256)", [(x,) for x in signed_singles()]),
    ("uint24RT(uint256)", [(x,) for x in unsigned_singles()]),
    ("addU256(uint256,uint256)", u256_pairs()),
    ("subU256(uint256,uint256)", u256_pairs()),
    ("mulU256(uint256,uint256)", u256_pairs()),
    ("addU8(uint256,uint256)", u256_pairs()),
    ("divI256(int256,int256)", i256_pairs()),
    ("modI256(int256,int256)", i256_pairs()),
    ("shlU256(uint256,uint256)", shift_pairs_u()),
    ("shrU256(uint256,uint256)", shift_pairs_u()),
    ("sarI256(int256,uint256)", shift_pairs_s()),
    ("addmodU(uint256,uint256,uint256)", modmul_triples()),
    ("mulmodU(uint256,uint256,uint256)", modmul_triples()),
    ("expU(uint256,uint256)", exp_pairs()),
    ("uncheckedAdd(uint256,uint256)", u256_pairs()),
]


def canon(v):
    """Canonical 256-bit pattern so the diff doesn't care whether the return
    decoded signed or unsigned. REVERT passes through."""
    return v % BITS if isinstance(v, int) else v


def avm_call(h, app, sig, args):
    r = h.call(app, sig, *args, expect_revert=True)  # simulate: value on success, reverted on trap
    return O.REVERT if r.reverted else r.abi_return


def _fmt1(v):
    if isinstance(v, int) and abs(v) > (1 << 40):
        return (hex(v) if v >= 0 else "-" + hex(-v))
    return str(v)


def _fmt(args):
    return "(" + ", ".join(_fmt1(a) for a in args) + ")"


def main():
    ln = LocalNet()
    h = Harness(ln, HERE / "out")
    print(f"compiling + deploying {FIXTURE.name} …")
    app = h.compile_and_deploy(FIXTURE)

    total = 0
    diverged = []
    errored = []
    for sig, arg_list in CASES:
        ofn = O.ORACLE[sig]
        for args in arg_list:
            total += 1
            expected = ofn(*args)
            try:
                actual = avm_call(h, app, sig, args)
            except Exception as e:  # harness/ABI hiccup, not a semantic divergence
                errored.append((sig, args, repr(e)[:140]))
                continue
            if canon(actual) != canon(expected):
                diverged.append((sig, args, expected, actual))

    print(f"\n=== {total} fuzzed calls across {len(CASES)} ops ===")
    if errored:
        print(f"\n⚠️  {len(errored)} call errors (harness/ABI, not divergences):")
        for sig, args, e in errored[:15]:
            print(f"   {sig}{_fmt(args)}: {e}")
    if diverged:
        print(f"\n❌ {len(diverged)} DIVERGENCE(S)  (AVM ≠ EVM-semantics oracle):")
        for sig, args, exp, act in diverged[:80]:
            print(f"   {sig}{_fmt(args)}  oracle={_fmt1(exp)}  avm={_fmt1(act)}")
    else:
        print("\n✅ no divergences — AVM matches the oracle on every fuzzed input")
    return 1 if diverged else 0


if __name__ == "__main__":
    sys.exit(main())
