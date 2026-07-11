# tiny-fuzzing-oracle — differential-fuzzing spike

A WIP tool, **not** part of the semantic suite. It started as a spike to answer one
question cheaply: *does input-fuzzing the known-risky codecs against an EVM-semantics
oracle surface real divergences?* It did (found the shift≥256 revert bug, fixed in
`db0ff47aff`), so it grew a **live-EVM mode** (`fuzz_evm.py`, below) that diffs the AVM
against a real solc+py-evm over arbitrary fixtures. Two layers:

- **hand-oracle spike** — `fuzz.py` + `oracle.py` (no EVM needed; the fast path).
- **live-EVM** — `fuzz_evm.py` + `evm_oracle.py` (real solc+py-evm; ABI-driven, any fixture).

## The idea

puya-sol diverges from the EVM *by design* almost everywhere at the byte level
(ARC4 encoding, 32-byte addresses, sha512_256 selectors, microAlgo balances, no
blockhash/gas). A naive "diff the raw output" tester drowns in that noise.

The escape: the **pure/view computational subset** — `intN` casts, checked
arithmetic, signed division, abi round-trips — has *no* by-design divergence. The
ABI-decoded return value should equal the EVM's, full stop. That's also exactly
where the real bugs have historically lived (int24 sign-extension, `signextend`,
checked-overflow edges, the biguint width trim). Clean signal, on the bug-dense
surface.

So: fuzz **boundary inputs** (0, ±1, type min/max, `2^N-1`, `2^N`, the sub-word
truncation edges) through such functions, run them on the AVM, and diff against a
Python model of the EVM result. No EVM needed for the spike — the oracle *is* the
spec.

## Layout

- `contracts/codec_probe.sol` — functions with **standard** ABI I/O (`int256`/
  `uint256`) that exercise the divergence-prone logic internally (sub-word
  truncate+widen, checked add/sub/mul, signed div/mod, abi round-trip).
- `oracle.py` — the EVM-semantics reference. `REVERT` is the trap sentinel.
- `fuzz.py` — boundary generators + the diff loop. Drives the semantic-test
  framework programmatically (`LocalNet` + `Harness`), simulates each call
  (`expect_revert=True` → value on success, reverted on trap), and canonicalizes
  both sides to the 256-bit pattern so signed/unsigned decode can't cause a false
  diff.

## Run

```
# needs localnet up + a built ./build/puya-sol (same prereqs as the suite)
python tests/WIP/tiny-fuzzing-oracle/fuzz.py
```

Exit 0 = no divergence; exit 1 = at least one (printed as `oracle=… avm=…`).

## Findings (first run, 1153 cases / 18 ops)

The spike paid off on the first extended run. Two kinds of result, both the point:

1. **Real divergence — unsigned `<<` / `>>` by shift ≥ 256 reverts (should be 0).**
   64/64 cases of `x << s` / `x >> s` with `s ∈ {256, 257, 300, 2²⁵⁶-1}` revert on
   the AVM; Solidity truncates (shifts aren't overflow-checked) so the EVM yields
   `0`. Even `0 << 256` reverts. Root cause: `buildBigUIntShift`
   (`src/builder/sol-eb/BigUIntMathHelpers.cpp`) builds `2^shift` via
   `setbit(bzero(32), 255 - shift, 1)`; for `shift ≥ 256` the `255 - shift` uint64
   subtraction underflows → out-of-range setbit index → AVM panic. The assembly
   `handleShl`/`handleShr` and the signed-SAR path both clamp + guard with
   `(shift < 256) ? expr : 0`; this unsigned operator path is the one that doesn't.
   `sar` (signed `>>`) correctly did **not** diverge, which localized it instantly.

2. **Oracle-modeling lesson — model Solidity, not the raw VM.** The first run also
   flagged `addmod`/`mulmod` with `m == 0`. The raw EVM opcode returns 0, but the
   Solidity builtin inserts `assert(k != 0)` (since 0.5.0) → both sides revert, so
   they actually *agree*. The oracle was modeling the opcode; fixed to model
   Solidity. After the fix the divergence set is exactly the 64 real shift cases.

## Live-EVM mode — `fuzz_evm.py` (ABI-driven, any fixture, no hand-modeling)

The spike earned its keep, so the hand-written `oracle.py` was promoted to a **real
solc + py-evm oracle**. Point it at any `.sol` with integer pure/view functions:

```
python tests/WIP/tiny-fuzzing-oracle/fuzz_evm.py [fixture.sol] [--max-per-fn N]
```

- `evm_oracle.py` runs in a dedicated venv (`.evmvenv`: `solcx` + `py-evm`/`eth-tester`
  + `web3`, isolated so it can't disturb the algosdk test env). It compiles the fixture
  with solc, deploys on an in-process py-evm chain, and executes a JSON batch of calls —
  returning each result as `value` or `revert` (a Solidity revert/require/assert/Panic
  surfaces as `ContractLogicError`). It also has an **introspect** mode that returns the
  function table.
- `fuzz_evm.py` (framework env) introspects the fixture, **auto-generates boundary inputs
  per param type** (full per-param sweep to catch single-param edges like `shift==256`,
  plus a capped key-value cartesian for interactions like overflow), runs them on the AVM
  *and* the live EVM via the subprocess, and diffs (canon to the 256-bit pattern). `bool`
  / non-integer params → the function is skipped (scalar filter).

Setup (one time): `python -m venv .evmvenv && .evmvenv/bin/pip install "eth-tester[py-evm]"
py-solc-x web3` (needs network for the package + first-time solc download).

**Validated (2026-06-17):** `codec_probe.sol` → 18 fns, 760 auto-generated calls, **0
divergences**; `arith_probe.sol` → 17/18 fns fuzzable (bitwise, signed/unsigned compares,
checked negate/abs, exotic int40/uint96 casts, sub-word `**`, modulo — none hand-modeled),
561 calls, **0 divergences**. The AVM matches a real solc+EVM on every boundary input.

## Scope / honesty

- Covers the computational subset only — says nothing about address/balance/selector/
  cross-call paths (those need a by-design allowlist of the EVM_DIVERGENCE.md FINE rows).
- `oracle.py` (the original hand spec) is kept for the no-EVM-needed path; `fuzz_evm.py`
  is the live-EVM path and the one to grow. A bug in the hand oracle reads as a false
  divergence — the live EVM removes that risk.
- ABI-driven gen currently fuzzes scalar `intN`/`uintN` params; arrays/structs/strings and
  multi-return tuples would extend it. Next reach: run it over the vendored semantic-test
  corpus (filtered to computational fns) to auto-scan for divergences.
