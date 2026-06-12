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

> **STATUS 2026-05-30 — all 8 recommended hard-errors LANDED.** 9
> `warning()`→`error()` swaps across 7 files. Full -n2 suite after the change:
> 1192 passed / 81 failed / 49 xfailed. The +6 vs the v330 baseline (75 failed)
> are **honest flips** — 6 tests whose contracts genuinely hit a now-hardened
> path and so fail to compile (verified individually: all 6 are
> `puya-sol exited 1` compile errors with the new messages, not runtime/flake
> failures). They are **xfailed** in this change (create2-sweep style), so the
> net pass count is unchanged. The flips and their trigger:
> - `constructor::test_callvalue_check` — Yul `create`
> - `events::test_event` — Yul `log3`
> - `inlineAssembly::test_optimize_memory_store_multi_block_bugreport` — Yul `log0`
> - `various::test_codebalance_assembly` — Yul `balance`
> - `inlineAssembly::test_keccak256_optimization` — `calldataload` unresolvable offset
> - `inlineAssembly::test_keccak256_optimizer_bug_different_memory_location` — same
>
> (4 of the 6 hit hard-error #8, the catch-all unknown-Yul-builtin guard; 2 hit
> #7, calldataload-unknown-offset.) The remaining **6 FIX** and **12 FINE** items
> below are unchanged.
>
> **UPDATE 2026-05-31 — four more hard-errors landed since (items 9–12 below):**
> `tx.origin` (`d3fc60bdb`), the code-introspection family incl. `extcodesize`/
> `extcodehash` (`d383b4921`), and `blockhash(n)` (`b4c24ba1c`). `block.chainid`
> and computed-slot storage aliasing were made **warn-level** (`9443b5150`). One
> warn-level item remains a pending decision: the unmapped-type fallback (see
> Next steps 2b). Verdict-table rows below updated accordingly.

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

## Encoding model (design rule + known seams)

**Rule (2026-06-12, confirmed with maintainer): the internal encoding is
ALWAYS ARC4.** Everything in the typed AWST world is ARC4/native — params
decode from ARC4 app args, returns ARC4-encode (`0x151f7c75` log),
cross-contract calls pass ARC4 app args, aggregates carry uint16 length
prefixes, scalars are uint64/biguint. EVM byte-layout may exist ONLY where
Solidity semantics make the bytes observable to the contract:

| EVM-shaped artifact | Bridge (ARC4 ↔ EVM) |
|---|---|
| `abi.encode*` output / `abi.decode` input | `builder/abi/` encodeArgsHeadTail / AbiDecode |
| revert payload logs (Error/custom) | RevertBlob builders |
| assembly calldata view | SyntheticCalldataOps `__cd_blob` |
| assembly memory (mload/mstore words) | blob memory model |
| storage slot packing (`.slot` asm compat) | StorageMapper |
| asm `return(o,s)` of ABI-built arrays | EIP-2330 stitch (MemoryOps) |

A future `--evm-compat` flag could flip selected bridges (selector hashing,
calldata canonicalisation); until then the table above is the whole list —
new EVM-shaped bytes anywhere else is a bug.

**Known seams violating the rule (queued, ordered):**

1. **`.call(payload)` end-to-end incoherence.** `abi.encodeWithSignature`
   builds keccak selector + EVM head/tail args
   (`builder/abi/AbiSelectorCalldataBuilder.cpp`); non-self `.call(data)`
   splits that blob `[selector, rest]` and forwards verbatim to the callee
   router (`builder/itxn/InnerCallHandlers.cpp`) — but puya-sol routers
   dispatch on sha512_256 selectors and expect ARC4 args. Such a payload can
   never match a puya-sol callee (self-calls only work because they are
   pattern-rewritten to direct subroutine calls). Fix: one bridge that
   re-encodes toward puya-sol callees (sha512_256 + ARC4).
2. **Selector hash split inside the `abi.*` family.** `encodeCall`, custom
   errors, events, methods → sha512_256 (`MethodConstant`);
   `encodeWithSignature`/`encodeWithSelector` → keccak. Two `abi.encode*`
   spellings of the same function produce different bytes. Align with the
   sha512_256 ruling (or document keccak as a raw-bytes escape hatch).
3. **`abi.decode` wrong-shape fallbacks.** ✅ DONE `d5f24791d5` —
   struct-field fallback is a hard error; the top-level corrupt-input
   fallback keeps its runtime trap (tests rely on it) with a scoped
   compile-log warning.
   Found during the same inventory and fixed: zero-argument events
   hand-rolled a keccak selector (`b4cbff4cf4` → MethodConstant) — events
   with fields were already sha512_256 via puya Emit. Still open in the
   same class: `f.selector` (SolSelectorAccess) returns keccak bytes —
   folded into seam #2's decision.
4. **bytes/string length-prefix re-framing is scattered.** ARC4 uint16 vs
   EVM 32-byte length word conversions are hand-rolled per assembly site;
   the >4KB blob model is ARC4-flat while asm expects EVM length-prefixed
   (multi-slot Phase B v2). Route through one named bridge.

## Recommended immediate hard-errors (ordered by danger)

All are localized `warning()`→`error()` swaps; no architectural change. The key
consistency gap: `create2` / salt / high-level `.delegatecall` are already hard
errors, but their **Yul/assembly-level twins and the precompile/staticcall
fallbacks are not** — that is where remaining silent divergence concentrates.

All ✅ LANDED in this change (2026-05-30). Line numbers below are
pre-change (HEAD `69258d852`); each is now a `Logger::error()`.

1. ✅ **`PrecompileHandlers.cpp:232` — `ec_pairing` (0x08) dynamic input size →
   stores `true`.** A zk/Groth16/pairing verifier on this path *always verifies*
   → any proof accepted. **Highest fund-theft potential.** (Constant-size path is
   correctly implemented; only the dynamic fallback is unsound.) *Verified.*
2. ✅ **`CoreTranslation.cpp:687` + `StatementOps.cpp:619` — Yul `delegatecall` →
   returns success-1 / no-op.** The high-level form is already hard-errored; the
   assembly twins leak through. Proxy/upgrade patterns silently no-op. *Verified.*
3. ✅ **`PrecompileDispatch.cpp:101` — precompile call, dynamic offsets, no runtime
   handler (0x01/0x03/0x09/0x0a) → success=1, no output written.** Code reads
   uninitialized output as if ecRecover/modexp ran; gates crypto checks.
4. ✅ **`InnerCallHandlers.cpp:690` — `address.staticcall(data)` fallback →
   `(true, "")`.** `require(ok)` passes spuriously; decoded returndata is all
   zeros. (Precompiles 0x01–0x08 *are* handled; only the genuine fallthrough is
   unsound.) NOTE: the feed-in `InnerCallShapes.cpp:395` (unimplemented-precompile
   path) was left as a `warning()` — it returns `nullptr` which funnels into this
   `:690` site, so the hard error already covers it; promoting it too is a
   harmless future cleanup.
5. ✅ **`DataOps.cpp:397` — `keccak256` sub-32B, unknown memory slot →
   `keccak256(bzero(32))`.** Wrong-but-deterministic hash poisons
   commitments / EIP-712 digests / Merkle leaves / mapping keys.
6. ✅ **`MemoryOps.cpp:505` — assembly `return` of a scalar where the fn returns an
   array → empty array.** Caller silently gets `[]`.
7. ✅ **`DataOps.cpp:73` — `calldataload` at unknown offset (no synthetic blob) →
   0.** Silently zeros a real input word (amount/recipient/selector).
8. ✅ **`CoreTranslation.cpp:757` — unknown Yul builtin (fallthrough) → 0.**
   Catch-all silent-zero; hard-error to surface every future gap.

### Additional hard-error landed later

9. ✅ **`tx.origin` → was silently aliased to `msg.sender` (`txn Sender`).** Two
   paths, both now hard-errored: the high-level intrinsic
   (`IntrinsicMapper.cpp`) and the Yul-assembly `origin()` builtin
   (`CoreTranslation.cpp`, previously sharing a branch with `caller()`). On EVM
   `tx.origin` is the EOA that started the transaction, distinct from
   `msg.sender`/`caller()` (the immediate caller); AVM has no
   transaction-origin concept, so the only value available is `txn Sender` =
   `msg.sender`. The old mapping made `tx.origin == msg.sender` (the classic
   "reject contract callers" guard) **always true** and `!=` always false —
   silently inverting access-control logic. **`msg.sender`/`caller()` are NOT
   affected** — `txn Sender` is the correct AVM analog of the immediate caller
   (including the cross-contract case, where it is the calling app's account).
   Flips one test (xfailed): `state::test_tx_origin`.

10. ✅ **EVM code-introspection family → hard-errored (both high-level & Yul-asm),
   EXCEPT the self case.** `extcodesize` (asm) returned 1 ("everything is a
   contract" — silently makes `extcodesize(a) > 0` EOA-vs-contract guards always
   true); `extcodehash` (asm) used a fragile `addr > 100` heuristic to guess
   "is this `address(this)`?" and otherwise returned a wrong-but-deterministic
   hash; `address(other).code` derived an app id from address bytes
   (unreliable / panics on non-existent app); `address(other).codehash`
   returned `bytes32(0)`. All four arbitrary-address paths are now hard errors.
   **Deliberately KEPT working** (verified, unaffected): `address(this).code`,
   `address(this).codehash` (computed correctly via `app_params_get` on the
   current app), compile-time `address(N).code/.codehash` literals, and Yul
   `caller()`. `extcodecopy` has no handler → already hits the unknown-builtin
   hard error (#8). Flips two tests (xfailed): `various::test_codehash_assembly`
   (asm `extcodehash`), `shanghai::test_evmone_support` (`address(other).code`).

11. ↔ **`selfdestruct` — deliberately KEPT (not a hard error).** Unlike the
   above, it is *not* a silent-wrong stub: `BuiltinCallables.cpp::handleSelfdestruct`
   emits a real inner payment with `CloseRemainderTo = beneficiary`, sweeping
   the app's balance — a faithful model of **post-Cancun** `selfdestruct`, which
   only transfers funds and does **not** delete the contract. The one divergence
   is that the AVM app is likewise not deleted (matches post-Cancun EVM). Left
   working by design; classified **FINE**.

12. ✅ **`blockhash(n)` → hard-errored (both high-level & Yul-asm).** Both paths
   (`SolBuiltinCall.cpp::handleBlockhash` and the Yul `blockhash` builtin in
   `CoreTranslation.cpp`, split out from the shared `blockhash`/`blobhash`
   branch) mapped to `block BlkSeed(Round-2)` — a per-round VRF seed that
   **ignores the round argument `n`** and panics for out-of-window rounds. That
   is a wrong value *and* a wrong failure mode, so a `blockhash`-based
   commitment or RNG silently diverges. AVM has no block-hash opcode and no
   faithful equivalent → refuse to compile. `blobhash` is **deliberately kept**
   as a stand-in (this change was scoped to `blockhash`). Flips three tests
   (xfailed): `builtinFunctions::test_blockhash`, `state::test_blockhash_basic`,
   `state::test_uncalled_blockhash`. NOT flipped: `test_blockhash_shadow_resolution`
   — its contract defines a user `blockhash` that shadows the builtin, so it
   never reaches the builtin lowering and still compiles.

   (NB: an aggregate used as a value in inline assembly is **not** a permanent
   divergence — it is its Yul memory pointer, which the AVM can model via the
   linear-memory blob. `ensureBiguint`'s hard-error on aggregates is a
   *temporary backstop*; the real fix is type-dispatched aggregate→memory-pointer
   resolution in the assembly handlers, backed by blob-promotion of
   assembly-used local aggregates. Tracked separately; left as real test
   failures, not xfailed.)

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
| `CoreTranslation.cpp:367` | `extcodesize` (arbitrary addr) | — (refused; was 1) | code size (0=EOA) | ✅ **HARD-ERROR** (#10) | — |
| `CoreTranslation.cpp:383` | `extcodehash` (arbitrary addr) | — (refused; was 0/keccak("")) | keccak(code) | ✅ **HARD-ERROR** (#10) | — |
| `CoreTranslation.cpp:586/521/528` | `codesize`/`difficulty`/`prevrandao` | harness sentinels | real values | **FINE** (test shim) | Low |
| `CoreTranslation.cpp:555` | `coinbase`/`gasprice`/`basefee`/`blobbasefee` | 0 | real values | **FINE** | Low (no AVM analog) |
| `CoreTranslation.cpp:727/752` | `calldatasize`/`calldatacopy` no-blob path | 0 / no-op | real calldata | **FINE** in no-blob path | Low-Med |
| `CoreTranslation.cpp:644` | `tstore` in expr ctx | no-op | (n/a — stmt only) | **FINE** (unreachable) | Low |
| `CoreTranslation.cpp:652` | `call`/`staticcall` in pure-expr ctx | returns 1 | (n/a) | **FINE** (real calls elsewhere) | Low |
| `DataOps.cpp:360` | `keccak256` sub-32B, known slot | hashes real bytes | hash of region | **FINE** (over-cautious warn) | Low |
| `AssemblyBuilder.cpp:633` | non-scalar in asm arithmetic | → biguint(0) | EVM pointer math | **FINE** (no AVM meaning) | Low |
| `SolMetaTypeAccess.cpp:123` | `type(C).creationCode/runtimeCode` | 32 zero bytes | bytecode | **FINE** (consumers hard-errored) | Low |
| `SolAddressProperty.cpp:265/273` | `.codehash` / other addr props | 0 / empty | real codehash | **FINE** (`this.codehash` correct) | Low |
| `SolBuiltinCall.cpp` / `CoreTranslation.cpp` | `blockhash(n)` | — (refused; was BlkSeed(Round-2), ignored `n`) | hash of block n | ✅ **HARD-ERROR** (#12) | — |
| `TypeMapper.cpp:173` | unmapped Solidity type | falls back to `bytes` | (n/a) | **FIX (selective, PENDING)** | Med (slices) / Low (meta-types) |

Confirmed observability-only (correctly FINE, not tabled): the `block.*` / `tx.*`
/ `address.balance` network-semantics warning family (`SolIntrinsicAccess.cpp`,
`IntrinsicMapper.cpp`, `SolBuiltinCall.cpp`), `blobhash` /
`ripemd160` documented cross-chain approximations (`blockhash` is now a
hard error — see #12), and all `splitter/*`,
`NodeBuilder`, `AsaIntrinsics`, `AwstWalker` compiler-internal diagnostics.

---

## Next steps

1. ✅ DONE — landed the 8 hard-errors (9 `warning`→`error` swaps, each with a
   one-line rationale in the error message) + xfailed the 6 honest flips they
   produced (see status note up top).
2. Tighten the 6 FIX sites where a real AVM mapping exists
   (`PrecompileDispatch.cpp:194` → `handleAppCall` is the highest-value).
2b. **Unmapped-type selective hard-error (DECISION PENDING).** `TypeMapper.cpp:173`
   currently warns + falls back to `bytes` for any unmapped type. A blanket
   `warning`→`error` flip regresses ~58 tests: ~27 are harmless **meta-types**
   that carry no runtime value (`type(library L)`, `type(struct …)`, `type(enum …)`,
   `type(contract D)`, `module "…"`, `abi`, `inaccessible dynamic type` — real ops
   on these go through dedicated paths), and 31 are value-carrying **slice** types
   (`bytes slice`, `uint256[] slice` from `x[a:b]`). The safe fix errors only on
   genuinely value-carrying unmapped categories (or special-cases the meta-types
   above the catch-all). Needs maintainer sign-off on scope.
3. Longer arc: generative **differential testing** (compile → run on evmone +
   AVM → diff, with an allowlist for the by-design FINE divergences above) to
   catch the silent-divergence class systematically rather than one incident at
   a time.
