# EVM ↔ AVM Divergence Manifest

**Purpose.** puya-sol compiles audited EVM Solidity to a *different* VM (Algorand
AVM) that holds real funds on an immutable ledger. The dangerous failure mode is
**silent semantic divergence**: code that compiles cleanly but behaves
differently from EVM. This document is the honest inventory of where that can
happen and what the compiler does about each case.

**Philosophy (project rule).** An unsupported EVM feature must **fail loudly** (a
compile-time hard error), never silently stub to a wrong value. Already enforced
this way: `create2`, `new C{salt:}()`, high-level `.delegatecall`, public-library
delegatecall.

**Mechanism.** `Logger::error()` increments an error count; `main.cpp:1254`
(`if (logger.hasErrors()) return 1;`) runs **before** the puya backend, so any
`error()` aborts the build with no TEAL emitted. `Logger::warning()` only prints
and the build proceeds to bytecode. Therefore "make it a hard error" =
change `warning()` → `error()` at the site (the stub's return value then becomes
dead code, exactly like the existing `create2` pattern at
`CoreTranslation.cpp:701`).

**Scope.** Of 67 `warning()` sites in `src/`, 26 are value-returning /
control-flow-affecting (the rest are observability-only network-semantics notes
or compiler-internal diagnostics). Verdict tally over those 26:
**8 HARD-ERROR, 6 FIX, 12 FINE.** (Audit 2026-05-29, HEAD `69258d852`. Top-2
findings + the abort mechanism were spot-verified against source by hand.)

---

## Recommended immediate hard-errors (ordered by danger)

All are localized `warning()`→`error()` swaps; no architectural change. The key
consistency gap: `create2` / salt / high-level `.delegatecall` are already hard
errors, but their **Yul/assembly-level twins and the precompile/staticcall
fallbacks are not** — that is where remaining silent divergence concentrates.

1. **`PrecompileHandlers.cpp:232` — `ec_pairing` (0x08) dynamic input size →
   stores `true`.** A zk/Groth16/pairing verifier on this path *always verifies*
   → any proof accepted. **Highest fund-theft potential.** (Constant-size path is
   correctly implemented; only the dynamic fallback is unsound.) *Verified.*
2. **`CoreTranslation.cpp:687` + `StatementOps.cpp:619` — Yul `delegatecall` →
   returns success-1 / no-op.** The high-level form is already hard-errored; the
   assembly twins leak through. Proxy/upgrade patterns silently no-op. *Verified.*
3. **`PrecompileDispatch.cpp:101` — precompile call, dynamic offsets, no runtime
   handler (0x01/0x03/0x09/0x0a) → success=1, no output written.** Code reads
   uninitialized output as if ecRecover/modexp ran; gates crypto checks.
4. **`InnerCallHandlers.cpp:690` (+ feed-in `InnerCallShapes.cpp:395`) —
   `address.staticcall(data)` fallback → `(true, "")`.** `require(ok)` passes
   spuriously; decoded returndata is all zeros. (Precompiles 0x01–0x08 *are*
   handled; only the genuine fallthrough is unsound.)
5. **`DataOps.cpp:397` — `keccak256` sub-32B, unknown memory slot →
   `keccak256(bzero(32))`.** Wrong-but-deterministic hash poisons
   commitments / EIP-712 digests / Merkle leaves / mapping keys.
6. **`MemoryOps.cpp:505` — assembly `return` of a scalar where the fn returns an
   array → empty array.** Caller silently gets `[]`.
7. **`DataOps.cpp:73` — `calldataload` at unknown offset (no synthetic blob) →
   0.** Silently zeros a real input word (amount/recipient/selector).
8. **`CoreTranslation.cpp:757` — unknown Yul builtin (fallthrough) → 0.**
   Catch-all silent-zero; hard-error to surface every future gap.

---

## Full verdict table

| Site | Feature | Current stub | EVM behavior | Verdict | Risk |
|---|---|---|---|---|---|
| `PrecompileHandlers.cpp:232` | `ec_pairing` dyn size | stores `true` | real pairing check | **HARD-ERROR** | Critical (any proof accepted) |
| `CoreTranslation.cpp:687` | Yul `delegatecall` (expr) | returns 1 | delegated exec | **HARD-ERROR** | Critical (control flow) |
| `StatementOps.cpp:619` | Yul `delegatecall` (stmt) | no-op | delegated exec | **HARD-ERROR** | Critical |
| `PrecompileDispatch.cpp:101` | precompile call, dyn offs, no RT handler | success=1, no output | runs precompile | **HARD-ERROR** | High (crypto checks) |
| `InnerCallHandlers.cpp:690` | `address.staticcall` fallback | `(true,"")` | `(success,data)` | **HARD-ERROR** | High |
| `DataOps.cpp:397` | `keccak256` sub-32B, unknown slot | `keccak(bzero(32))` | hash of region | **HARD-ERROR** | High (commitments) |
| `MemoryOps.cpp:505` | asm-return scalar → array fn | empty array | encoded array | **HARD-ERROR** | High |
| `DataOps.cpp:73` | `calldataload` unknown off | 0 | calldata word | **HARD-ERROR** | High |
| `CoreTranslation.cpp:757` | unknown Yul builtin | 0 | (unknown) | **FIX→hard-err** | Medium |
| `PrecompileDispatch.cpp:194` | call to non-precompile const addr | success=1, no-op | external call | **FIX** (route to `handleAppCall`) | Medium |
| `BitwiseShiftOps.cpp:257` | `signextend` non-const `b` | returns x unchanged | sign-extend at b | **FIX** | Medium (rare) |
| `InnerCallShapes.cpp:395` | `staticcall` to 0x09 etc. | nullptr → 690 stub | runs precompile | **FIX/hard-err** | Medium |
| `MemoryOps.cpp:452` | void fn asm `return(off,sz>0)` | emits as log | raw return bytes | **FIX** | Low/Med |
| `SolInternalCall.cpp:547` | uninit/unsupported fn-ptr call | `assert(false)` (reverts) | reverts (uninit) | **FIX** (support dispatch) | Low (fails loud, not silent) |
| `CoreTranslation.cpp:370` | `extcodesize` | returns 1 | code size (0=EOA) | **FINE/borderline** | Low-Med (`>0` always true) |
| `CoreTranslation.cpp:383` | `extcodehash` | 0 / keccak("") / self | keccak(code) | **FINE** (stub) | Low |
| `CoreTranslation.cpp:586/521/528` | `codesize`/`difficulty`/`prevrandao` | harness sentinels | real values | **FINE** (test shim) | Low |
| `CoreTranslation.cpp:555` | `coinbase`/`gasprice`/`basefee`/`blobbasefee` | 0 | real values | **FINE** | Low (no AVM analog) |
| `CoreTranslation.cpp:727/752` | `calldatasize`/`calldatacopy` no-blob path | 0 / no-op | real calldata | **FINE** in no-blob path | Low-Med |
| `CoreTranslation.cpp:644` | `tstore` in expr ctx | no-op | (n/a — stmt only) | **FINE** (unreachable) | Low |
| `CoreTranslation.cpp:652` | `call`/`staticcall` in pure-expr ctx | returns 1 | (n/a) | **FINE** (real calls elsewhere) | Low |
| `DataOps.cpp:360` | `keccak256` sub-32B, known slot | hashes real bytes | hash of region | **FINE** (over-cautious warn) | Low |
| `AssemblyBuilder.cpp:633` | non-scalar in asm arithmetic | → biguint(0) | EVM pointer math | **FINE** (no AVM meaning) | Low |
| `SolMetaTypeAccess.cpp:123` | `type(C).creationCode/runtimeCode` | 32 zero bytes | bytecode | **FINE** (consumers hard-errored) | Low |
| `SolAddressProperty.cpp:265/273` | `.codehash` / other addr props | 0 / empty | real codehash | **FINE** (`this.codehash` correct) | Low |
| `TypeMapper.cpp:173` | unmapped Solidity type | falls back to `bytes` | (n/a) | **FINE/latent FIX** | Low |

Confirmed observability-only (correctly FINE, not tabled): the `block.*` / `tx.*`
/ `address.balance` network-semantics warning family (`SolIntrinsicAccess.cpp`,
`IntrinsicMapper.cpp`, `SolBuiltinCall.cpp`), `blockhash` / `blobhash` /
`ripemd160` documented cross-chain approximations, and all `splitter/*`,
`NodeBuilder`, `AsaIntrinsics`, `AwstWalker` compiler-internal diagnostics.

---

## Next steps

1. Land the 8 hard-errors (8 localized `warning`→`error` swaps + xfail any tests
   that were only passing via the stub, as honest failures — same treatment as
   the create2 sweep). Each needs a one-line rationale in the error message.
2. Tighten the 6 FIX sites where a real AVM mapping exists
   (`PrecompileDispatch.cpp:194` → `handleAppCall` is the highest-value).
3. Longer arc: generative **differential testing** (compile → run on evmone +
   AVM → diff, with an allowlist for the by-design FINE divergences above) to
   catch the silent-divergence class systematically rather than one incident at
   a time.
