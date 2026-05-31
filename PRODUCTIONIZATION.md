# puya-sol Productionization Roadmap

> Status: **strong v0.x, not production-ready for real funds.** This is the plan to
> get there. Companion to [`EVM_DIVERGENCE.md`](./EVM_DIVERGENCE.md) (the
> per-site silent-divergence manifest) and
> [`tests/solidity-semantic-tests/CURRENT.md`](./tests/solidity-semantic-tests/CURRENT.md)
> (current test status). Derived from an 8-dimension production-readiness audit
> (2026-05-30).

## Verdict

puya-sol is an impressively broad, honestly-disclaimed compiler — ~90% of the
Solidity semantic suite passes single-threaded (1192/1322), 2840+ real-world
tests across OpenZeppelin / Uniswap V2+V4 / AAVE V4 / Morpho / Tornado / PRB-Math
compile and run, and the project has a genuine, recently-strengthened **fail-loud
safety culture** (create2, delegatecall, salted-new, and 8 more divergence stubs
are now compile-time hard errors).

**But it is not yet production-ready for MainNet funds, and the gap is structural,
not cosmetic.** The blockers are not the ~75 failing tests — they are the absence
of the machinery needed to *trust any output*:

- **No differential oracle.** Correctness rests entirely on hand-authored Python
  golden values written by the same author as the contract. "~90% pass" is a
  number measured against a homemade ruler, not a reference EVM.
- **No CI.** Regression gating is a human eyeballing 106 `results_v*.txt` files
  against a baseline ~126 versions stale. A stale tree already silently reverted
  three versions' worth of fixes once.
- **No provenance / immutable release.** A deployed contract's TEAL cannot be
  reproduced from, or attributed to, any named compiler version. The ARC56 stamp
  records only a mislabeled, version-mismatched backend.
- **A residue of silent approximations** — mostly closed as of 2026-05-31:
  ~~extcodesize→1~~ and ~~blockhash→wrong-round~~ are now hard errors
  (`d383b4921`, `b4c24ba1c`); chainid→1 now warns. Still open: `unmapped-type→bytes`
  (needs a *selective* hard error — see EVM_DIVERGENCE.md Next-steps 2b).
  By-design (not violations): selfdestruct-approx (faithful post-Cancun) and
  msg.sender→Txn.Sender (correct AVM analog of the immediate caller).

The just-landed 8 hard-errors (the `ec_pairing` "accept-any-proof" bug was the
scariest) prove the discipline works and is the right model — it simply hasn't
been driven to closure or backed by behavior-diffing.

## Definition of "production-ready v1"

> A third party can trust that a deployed app's TEAL matches its Solidity source,
> with every divergence from EVM either impossible-to-trigger-silently or
> explicitly flagged.

Concretely, ALL of:

1. **Fail-loud completeness** — zero silent semantic divergences in any
   fund-affecting path. Every unsupported / non-equivalent EVM feature is a
   compile-time hard error OR a test-pinned, documented intentional divergence —
   established by a sweep **independent of whether a `warning()` exists**
   (covering `return true`/`0`/`makeTrue`/`(true,"")`/`nullptr`-in-value-path/
   `bytes`-fallback), not just the 26 logged warning sites already audited.
2. **Differential oracle** — a mechanized harness runs identical calldata on a
   reference EVM (evmone/revm) AND on AVM and asserts byte-equal returndata/state,
   with a machine-checked allowlist encoding only the by-design FINE divergences
   (sha512_256 selectors, 32-byte addresses, mod-2²⁵⁶ wrap, BN254 G2 ordering).
3. **Reliable gate** — deterministic CI on every push: pinned toolchain + pinned
   `puya`/`solidity` submodules, runs the suite, BLOCKS on any regression vs a
   committed baseline. The flaky `-n2` path is fixed or CI is pinned
   single-threaded with a clean green/red signal.
4. **Provenance & reproducibility** — semver git tag + accurate ARC56
   `compilerInfo` stamping puya-sol commit + puya version + solidity version +
   effective `--evm-version` + source hash, so exact bytecode is reproducible and
   attributable.
5. **License + SECURITY.md + a user-facing SUPPORT/SEMANTICS doc** enumerating
   supported features and explicit non-goals.
6. **A release gate** that blocks shipping while any fund-loss-class item is open.

### Explicitly OUT of scope for v1 (documented non-goals, hard-errored or clearly flagged — never silently approximated)

- EVM flat-slot storage packing / assembly `.slot :=` (~15 tests)
- Full byte-addressable `mload`/`mstore8`/`mcopy` memory-pointer fidelity (~14 tests)
- keccak256-based function selectors (AVM uses sha512_256 — cross-chain selector
  compatibility is not offered)
- 20-byte address identity (AVM addresses are 32 bytes)
- CREATE2 / salted-new, runtime `delegatecall`, try/catch, `blockhash`,
  `extcodesize`/`address.code`
- Large snark/groth16 verifiers that exceed the AVM aggregate ceiling even after
  splitting

v1 is *"a trustworthy compiler for the supported subset,"* not *"a complete EVM."*

## Top risks (scariest first — weighted by fund-loss / silent-miscompilation)

1. **Silent miscompilation outside the audit's reach.** The divergence audit keys
   off `Logger::warning()` sites by construction; any handler returning
   `true`/`0`/`makeTrue`/`nullptr`/`bytes`-fallback with no log was never counted.
   `ec_pairing` (one un-swept stub → every zk/Groth16 proof accepted → total fund
   theft) proves the blast radius of a single missed site. Until a
   logging-independent sweep + a behavior-diffing oracle exist, nobody can assert
   the fund-loss class is empty.
2. **No differential oracle** — correctness vs hand-authored Python goldens. A
   systematic codegen error the author *also* mis-modeled in Python passes green.
3. **Known silent approximations** — mostly closed 2026-05-31. ~~`extcodesize→1`~~
   and ~~`blockhash→wrong-round`~~ are now hard errors; `chainid→1` now warns. The
   one remaining silent-wrong is `unmapped-type→bytes` (wrong storage/ABI layout),
   which needs a *selective* hard error (value-carrying types only — ~27 harmless
   meta-types must stay; see EVM_DIVERGENCE.md 2b). selfdestruct-approx is by-design
   (faithful post-Cancun).
4. **No CI + stale baseline** — any commit can quietly change emitted TEAL with no
   automated catch.
5. **No provenance / immutable release** — an unverifiable deployment is an
   unauditable one.
6. **`msg.sender` silently maps to `Txn.Sender`** with no note — under inner-txn /
   cross-contract invocation these differ, so a ported `onlyOwner` /
   `require(msg.sender==x)` can guard the wrong identity.
7. **Un-enforced splitter rekey trust assumption** — split contracts (Morpho,
   Uniswap V4) depend on a deploy-time rekey ritual the compiler does not enforce
   on-chain. Skipped or left-open → trust model breaks.
8. **Compile-time DoS + runtime-fit cliffs** — a 48-line contract hangs the
   backend (no CSE on repeated subexpressions); the 4KB stack-value cap can fail
   at *runtime* for runtime-grown dynamic state (a bare AVM revert after funds are
   live).

## Critical path (the must-do spine)

| # | Step | Effort | Why |
|---|------|--------|-----|
| 1 | **CI** (GitHub Actions): pinned-toolchain build + pinned submodules + single-threaded suite, FAIL on regression vs a fresh committed v332 baseline | M | Every other correctness investment is worthless without an automated ratchet. (*Deferred by maintainer for now.*) |
| 2 | Regenerate + commit a CURRENT baseline; wire `analyze_baseline.py` to exit non-zero on any newly-failing test | S | Makes the CI gate meaningful; the diffing machinery exists but is dead (stale baseline). |
| 3 | **Close the fail-loud holes the audit missed**: logging-*independent* sweep (`return true`/`0`/`makeTrue`/`(true,"")`/`nullptr`/`bytes`-fallback) across `sol-eb/`, `assembly/`, `splitter/`; hard-error or test-pin every fund-affecting one | M | The audit covered 26 of 67 logged sites; wrong-by-construction codegen that emits no warning is outside it. |
| 4 | **Provenance sidecar**: stamp ARC56 `compilerInfo` (puya-sol commit + puya version + solidity version + `--evm-version` + source hash) + emit exercised-divergence list; first semver tag + real `--version` | M | Today bytecode can't be reproduced from or attributed to a named version. |
| 5 | **Differential oracle**: drive the ~1322-contract corpus through evmone/revm AND AVM, diff returndata+state, allowlist only the FINE divergences. Prove first on keccak256-preimage / ecrecover | XL | The only thing that catches "silent and wrong" by *behavior* rather than by a hand-placed warning. Largest correctness multiplier. |
| 6 | **LICENSE + SECURITY.md + SUPPORT/SEMANTICS.md** ("supports X / hard-errors Y / does not support Z") | M | Legal right-to-use, a fund-theft disclosure channel, an authoritative feature contract. |
| 7 | **Determinism foundation**: fix `-n2` flakiness (isolated algod per worker OR offline golden-TEAL + AVM-simulate tier); switch compile-cache key from mtimes to content hashes | L | A flaky gate and an mtime cache both already let silent regressions through. |
| 8 | **Eliminate compile-DoS + runtime-fit cliffs**: fix the AWST repeated-subexpression blowup (hoist repeated reads to one materialized local), add a node-count/wall-clock compile budget, ensure every splitter cannot-fit path hard-errors | L | A 48-line contract hanging the backend is a build DoS; a splitter that warn-and-proceeds could emit an undeployable/wrong program. |

## Phasing

### Now (days)
- Regenerate + commit a CURRENT baseline; wire `analyze_baseline.py` to fail on regression.
- Flip the **genuinely-safe** silent approximations to hard errors (only those with
  no passing-test blast radius / not FINE-by-design — see caveat below).
- Add `LICENSE` + a one-paragraph `SECURITY.md`.
- Add `msg.sender`→`Txn.Sender` as a risk row in `EVM_DIVERGENCE.md` (silent
  fund-control identity hazard under inner txns).
- Re-anchor stale `EVM_DIVERGENCE.md`/source comment line numbers (several point at
  sites that are already hard errors, or describe behavior the live code no longer
  does — e.g. the RIPEMD-160 "returns zero bytes20" comments above the real
  subroutine call).
- Wrap `builder.build()` in an outer try/catch → located "internal compiler error
  — please report" + `return 1`.

### Next (the v1 push, weeks)
- Stand up CI (the keystone).
- Logging-independent silent-stub sweep across `sol-eb`/`assembly`/`splitter`.
- Accurate ARC56 provenance + first semver tag + `--version` + assert invoked puya
  version matches the pinned submodule.
- Build the differential oracle (evmone/revm vs AVM), seeded on the corpus.
- Fix suite determinism (isolated algod per worker OR offline golden-bytecode tier;
  content-hash cache + cold-cache CI run).
- Publish SUPPORT.md/SEMANTICS.md + per-compilation exercised-divergence sidecar.
- Fix the AWST blowup + compile budget; audit splitter cannot-fit paths.
- Pin the puya backend per release + backend-upgrade regression gate (puya 5.9
  optimizer already silently turned 4 tolerated shapes into runtime reverts).

### Later (post-v1 maturity)
- Generative/property fuzzing through the differential oracle; gate on codegen
  line/branch coverage.
- Land the 6 substantive FIX divergence items (PrecompileDispatch
  call-to-non-precompile → `handleAppCall` first).
- PC→Solidity-source-line source maps in ARC56 `sourceInfo`.
- Independent external security audit of codegen for high-value patterns (pairing
  verifiers, ERC4626 vaults, access control, token transfers).
- Reproducible/hermetic distribution (container w/ pinned submodules+boost; remove
  hardcoded `BOOST_ROOT`; GPL-3.0 implications of static libsolidity linking;
  document the puya-fork delta).
- Splitter maturity: reachability analysis (stop duplicating subroutines into
  every chunk); on-chain-enforce the deploy-time rekey.
- EVM-fidelity workstreams IF funded and IF they become v1 goals: flat-slot
  storage packing (~15 tests) and byte-addressable memory (~14 tests).

## The single highest-leverage first move

**Stand up CI gated on a fresh committed v332 baseline.** Every other correctness
investment — differential testing, fuzzing, the silent-stub sweep, any
verifiability claim — depends on an automated ratchet, and the gating machinery
(`analyze_baseline.py`) already exists; it is merely dead because the baseline is
126 versions stale. *(Deferred by maintainer; the next-best independent move is
the logging-independent silent-stub sweep — step 3.)*

## Caveat on "flip the silent approximations"

The audit listed several `warning()→error()` swaps as quick wins. Treat with care:
unlike create2 (which had **zero** passing tests, so flipping was free), some are
exercised by **currently-passing** contracts. UPDATE 2026-05-31: `extcodesize`
(arbitrary-addr) and `blockhash` were flipped anyway — the affected contracts were
xfailed in the same commits (`d383b4921`, `b4c24ba1c`), the create2 pattern. The
caution still holds for the rest, esp. `selfdestruct` (6 contracts) which is
classified **FINE/by-design** in `EVM_DIVERGENCE.md` (no AVM analog exists — the
stub is the only possible answer).
Flipping those would cause regressions AND is a **product decision** ("refuse
`Address.isContract()`-style code, or support it with a documented caveat?"), not a
safety no-brainer. Each candidate must be checked individually against (a) its
manifest classification and (b) its passing-test blast radius before flipping.
