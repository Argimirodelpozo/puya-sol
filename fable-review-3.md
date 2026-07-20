# fable-review-3 — src/builder correctness review

2026-07-19. Successor to `fable-review.md` (2026-07-02) and `fable-review-2.md` (2026-07-08).
Scope: **src/builder only** (~46k lines across 10 subdirectories), reviewed by seven parallel
full-file surveys (one per subdirectory cluster), findings then adversarially verified — the
highest-severity ones end-to-end against the built compiler (`--dump-awst` PoCs, and for the
contract/ cluster through the puya backend to TEAL). Verification legend:
**✅** = empirical PoC (AWST/TEAL inspected) · **✔** = code-path re-traced and confirmed in this
review · **○** = surveyed with line-cited evidence, not independently re-verified.

Unlike review-1/-2 (architecture-first), this is a bug harvest. ~90 raw findings consolidated
below into 4 criticals, 14 highs, and a medium/low tail, followed by the systemic patterns they
cluster into (Part V) — the recurring classes C1–C4 from review-1 all reappear, plus one new
class: **stale compile-time caches** (N1).

---

## Part I — Critical (silent miscompiles of common idioms)

> **STATUS 2026-07-19: all four criticals FIXED** (same-day follow-up session; each with an
> E2E localnet regression guard in `tests/puyasolRegression`; full suite 12f/1331p/109xf/28xp,
> failure set identical = zero regressions). #1 → [pay, appl] inner-group submit
> (`test_call_value_with_data_invokes_target`); #2 → single-assignment gate + mem-constant
> invalidation on writes/control-flow (`test_asm_const_cache_invalidation`); #3 → appendPendingTo
> at all three ctor-arg sites (`test_ctor_ternary_base_arg`); #4 → derived-first arg pass split
> from base-first body inlining (`test_postinit_transitive_ctor_args`).

### 1. `.call{value: X}(data)` silently drops the calldata ✅✔ — FIXED
`itxn/InnerCallHandlers.cpp:451` — any `.call` with a `{value:}` clause routes to
`handleCallWithValue`, which builds **only a payment itxn** and returns `(true, "")`. The guard
never checks that the data argument is empty (the comment says `.call{value: X}("")` but the
code matches `.call{value: X}(anything)`). The data expression is never even built, so its side
effects vanish too.
```solidity
(bool ok,) = target.call{value: 1e6}(abi.encodeWithSignature("deposit()"));
// compiles clean; sends payment; deposit() never runs; ok == true
```
This is the standard payable-interaction idiom in half of DeFi. Fix: only take the payment path
when the argument is a compile-time empty literal; otherwise build an app-call itxn that also
carries the payment (grouped pay + appcall), or hard-error.

### 2. Inline-asm constant caches are never invalidated — pointer-bump loops fold wrong ✅ — FIXED
`assembly/StatementOps.cpp:112-127` — `m_localConstants` is populated at `let` declarations
(including `= 0` for bare `let x`) and **never erased on assignment**; it is also
flow-insensitive (entries from branch/loop bodies persist). Both constant resolvers
(`AssemblyBuilder.cpp:590`, `DataOps.cpp:127`) then fold offsets/values with stale constants.
PoC-verified miscompiles:
```yul
let p := 0x80  mstore(p, 111)  p := add(p, 0x20)  mstore(p, 222)   // BOTH stores fold to offset 128
for { let i := 0 } lt(i, 3) { i := add(i, 1) } { mstore(add(0x80, mul(i, 0x20)), v) }
// single replace3 at constant 128 inside the loop — every iteration writes 0x80
```
Sibling with the same root cause ○: `m_lastMstoreValue` / `mem_0x<off>` content-constants
(`MemoryOps.cpp:555-571`) are also never invalidated nor branch-scoped, feeding the
compile-time keccak fold and `mload` folds with stale values (`DataOps.cpp:163-175, 236-249`).
Cheapest sound fix: pre-scan the Yul block and only admit locals **assigned exactly once** into
`m_localConstants`; kill the memory-content cache on any non-constant `mstore` and at every
branch/loop boundary (or restrict it to straight-line prefixes).

### 3. Base-constructor arguments bind before their pre-statements run ✅ — FIXED
`contract/ApprovalProgramBuilder.cpp:681` (Phase 1; also 721 Phase 2, 542 __postInit path) —
all three base-ctor-arg sites `build(*args[i])` and emit the param assignment **without
draining pre-pending statements**, unlike the fixed twins `ModifierInliner.cpp:265` /
`ModifierBodyInliner.cpp:280`. Verified: `constructor(int a) A(a > 0 ? a : -a)` emits
`x = __cond_0` *before* the if/else that assigns `__cond_0`; `D(5)` initializes the base with 0.
Any ternary / checked-negate / short-circuit ctor argument is affected.

### 4. `__postInit` evaluates transitive base-ctor args before intermediate params are assigned ✅ — FIXED
`contract/ApprovalProgramBuilder.cpp:520-584` — the postInit inlining loop walks base-most
first and evaluates each base's explicit args immediately, so for `D is C is A` with
`C(uint y) A(y+1)`, `y+1` reads `y` before `C`'s param assignment. Verified: `va = 1` instead
of `6`. The non-postInit path got exactly this fix (Phase 1/Phase 2 pre-evaluation,
comment at 647-649); the postInit twin never did. Classic C3.

---

## Part II — High

### Pending-statement sequencing cluster (sol-ast/stmts) — one invariant, five holes
The ContractContext pre/post-pending buffers are the effect-sequencing backbone, and statement
handlers disagree about draining them:

- **H1 ✔ `SolIfStatement` emits condition post-pendings after the if/else**
  (`stmts/SolControlFlow.cpp:29-57`). Post-pendings carry storage/memory write-backs
  (`SolInternalCall.cpp:651,674`) and `push/pop` box writes. `if (Lib.mutate(st) > 0) return;`
  — the write-back is emitted after the IfElse and never runs when the branch returns; branch
  bodies also read pre-write-back state.
- **H2 ✔ `SolEmitStatement` never drains anything** (`stmts/SolEmitStatement.cpp` — zero
  `Pending` references in the file). Pre-statements from arg builds (bounds asserts, eval-once
  temps, hoisted submits) leak into whichever statement translates next — potentially in a
  different function.
- **H3 ✔ do-while condition pendings are never captured** (`SolControlFlow.cpp:77`; the while
  and for paths capture at 129-130/192-193). The condition's pre-statements are drained by the
  first *body* statement — they execute at the TOP of the body while the test runs at the
  BOTTOM, one iteration apart. A body that never drains (e.g. bare `continue;`) leaks them out
  of the loop.
- **H4 ○ write-backs are post-pending, flushed after the statement**
  (`SolInternalCall.cpp:651,674` + `SolExpressionStatement.cpp:56`): `x = Lib.mutate(st) + st.f;`
  reads stale `st.f`; the identical initializer form flushes first
  (`SolVariableDeclaration.cpp:329`) — the two statement forms disagree.
- **H5 ✅ trailing bare `calldatacopy` in asm is dropped** (`assembly/StatementOps.cpp:520-696`
  generic fall-through queues the write on `m_pendingStatements`; `buildBlock` never drains at
  block end). PoC: the memory write is absent from the AWST. Args also built twice.

Systemic fix in Part V (T1).

### H6 ✔ `ParamMutationDetector` only sees `Assignment` — FIXED
> **2026-07-20:** detector now records `++`/`--`/`delete` (UnaryOperation) and `push`/`pop`
> member-call receivers; both consumers (callee augmentation + caller write-back/Copy guard)
> share it so they stay in lockstep. Known remaining gap (documented in the header): mutation
> via passing the param to ANOTHER mutating callee needs call-graph closure. Guard
> `test_param_mutation_incdec_writeback`.
`sol-ast/ParamMutationDetector.h:30` — `++`/`--`/`delete` and mutating member calls
(`p.arr.push(x)`, `pop`) are not recorded. Callee mutating only via `a[0]++` ⇒ classified
non-mutating ⇒ caller write-back skipped (`SolInternalCall.cpp:471`) *and* the arg-aliasing
Copy guard misfires. `inc(arr){arr[0]++;}` — mutation silently lost.

### H7 ✔ Storage-pointer reassignment in a conditional branch rebinds unconditionally — FIXED (fail-loud)
> **2026-07-20:** conditional reassignment (if/loop bodies, ternary/short-circuit arms — tracked
> via `ContractContext::conditionalDepth`, bumped by branch/loop builders and
> `buildScopedOperand`) is now a HARD ERROR; straight-line reassignment keeps working
> (guards `test_conditional_storage_ptr_reassign_fails_loud` / `..._still_works`). NEW FINDING
> while testing: **mutating through a ternary-INIT pointer is also broken pre-existing** —
> `uint[] storage p = c ? a1 : a2; p.push(x);` pushes into a materialized VALUE copy (length
> read back = 1 regardless of target); reads through the ternary alias are fine. Needs its own
> fix (runtime-selected handle); noted here, not yet fixed.
### (original H7 text) Storage-pointer reassignment in a conditional branch rebinds unconditionally
`exprs/SolAssignmentEarlyOuts.cpp:105-112` — `p = s2` lowers to compile-time
`setStorageAlias` + `VoidConstant` in the **flat** decl-id-keyed ScopeState; `SolIfStatement`'s
branch builder saves/restores only `terminated`. `uint[] storage p = a1; if (c) p = a2;
p.push(1);` always pushes to `a2`. Same property for `memoryAliases`/`funcPtrTargets` and the
tuple variant. (Mapping-key-param locals take the runtime-bytes path and are fine — the
value-typed alias path is the hole.)

### H8 ✔ Slot-handle fixed arrays: no bounds check + packed compound assign hits the wrong slot — FIXED
> **2026-07-20:** `SlotHandleAccess::boundsCheckIndex` (assert idx < length + EvalOnce pin,
> also closing M3's packed-read double-eval) wired into both SolIndexAccess slot paths and the
> SolAssignment intercept; the intercept now handles packed COMPOUND ops via packed-aware read
> → native-carrier checked arithmetic → sub-word write-back. Bonus find: the intercept never
> fired for ARRAY-typed locals at all (buildExpr(base) maps the declared arc4 type, not the
> biguint handle) — even PLAIN packed writes took the wrong-slot whole-word path; base is now
> constructed as the raw handle var. Guard `test_slot_handle_array_bounds_and_packed_compound`.
- `exprs/SolIndexAccess.cpp:183-274` — slot-handle element read/write emits no `idx < len`
  assert anywhere (the mapping-chain path does, `SolIndexAccessHandlers.cpp:260-308`).
  `uint[2] storage p; p[5] = 1` corrupts a neighboring state slot where EVM panics 0x32.
- `exprs/SolAssignment.cpp:200` — the packed-aware intercept bails on anything but plain `=`,
  so `uint8[8] storage p; p[3] += 1` falls to the generic path and read-modify-writes the whole
  word at `base+3` (unscaled) instead of byte 3 of slot `base`.

### H9 ✅ Compound `/=`,`%=` with biguint-backed signed LHS and narrower signed divisor — FIXED
> **2026-07-20:** shared `widenSignedCompoundRhs` converts the RHS to the target's canonical
> form before the compound compute, at ALL compound sites. Guard
> `test_compound_signed_mixedwidth_divisor` (also covers -=, *=, %=, and the ≤64-bit tier).
`sol-eb/AssignmentHelper.cpp:55` builds the RHS builder with the *target* type, so
`SolIntegerBuilder::binary_op` sign-extends the divisor from the **target** width
(`SolIntegerBuilder.cpp:168`), not its own: `int128 x; int16 y = -32768; x /= y;` reads the
divisor as +1.8e19. Verified by AWST diff against the plain-division form (which extends
from 16). Live residual of the "closed" signed-mixedwidth-div family — the fix assumed
`otherInt` carries the RHS's own type; on the compound path it carries the target's.

### H10 ✅ `encodeReturnValue` leaves non-literal ternary branches unencoded — FIXED
> **2026-07-20:** the in-place conditional path now requires BOTH branches to be literal
> tuples; call/nested-ternary branches fall through to the opaque-tuple spill. Guard
> `test_ret_ternary_encode`.
`sol-types/TypeCoercion.cpp:195-201` — for a conditional return value, `wrapItems` silently
no-ops when a branch is a call or nested ternary, yet the node is retyped to the wire tuple, so
the opaque-tuple spill fallback (line 205) is unreachable. Verified: raw biguint items flow
where arc4.uint256 is expected; puya accepts and emits TEAL ⇒ corrupt ABI return blob at
runtime (minimal-length biguint vs 32-byte word).

### H11 ✅ Unary `-`, `~`, and unsigned `**` duplicate side-effecting operands — FIXED (core)
> **2026-07-20:** EvalOnce at the unary builder dispatch (Not/Sub/BitNot; Inc/Dec/Delete keep
> their lvalue trees), Exp operands pinned on the unsigned binary path, and the enum
> range-check assert in SolAssignment (its statement twins already had the fix). Guard
> `test_eval_once_unary_pow_enum`. The "same class ○" tail (slice base, ecrecover/ecPairing
> inputs, fn-ptr expr, encodePacked, SolAddressBuilder compare, index pins) remains open —
> T2's builder-entry umbrella.
`exprs/SolUnaryOperation.cpp:745-751` passes the raw operand; `SolIntegerBuilder` references it
2-3× (assert + op). Verified by AWST call counts: checked `-g()` ⇒ **3** calls to `g`; `~g()` ⇒
2; `x ** f()` ⇒ 2 (EvalOnce is applied on the signed binary path only,
`SolBinaryOperation.cpp:269`). Same class ○: enum range-check assert re-evaluates a call-valued
RHS (`SolAssignment.cpp:158-171`, also the twins in SolExpressionStatement/SolEmitStatement),
slice lowering re-evaluates its base (`SolIndexAccess.cpp:437/488/562`), ecrecover/ecPairing
embed `_inputData` 4-12× (`itxn/InnerCallShapes.cpp:343-436`), fn-ptr `_ptrExpr` 3×
(`FunctionPointerBuilder.cpp:253-305`), encodePacked fixed-array N× + len==0 double-build
(`abi/AbiEncoderBuilder.cpp:241-293`), `SolAddressBuilder::compare` stored-side 2×
(`SolAddressBuilder.cpp:112-118`), compound-assign uint64 index / nested-call mapping keys
escape the existing pins (`SolIndexAccessHandlers.cpp:110-114, 245-254, 633`).

### H12 ✅ Yul evaluation-order and inlining bugs (assembly/)
- **Arg order**: call arguments translate left-to-right; Yul mandates right-to-left
  (`CoreTranslation.cpp:346-349`). PoC `sub(bump(1), bump(10))` shows left-first sequencing.
  Nested inlining additionally splices an earlier sibling's pending inline body into the later
  sibling's (drain-from-index-0).
- **Local capture**: inline expansion alpha-renames only params/returns
  (`UserFunctionOps.cpp:148-174`); body `let`-locals keep bare names — two helpers sharing a
  scratch name (`t`, `ptr` — Solady house style) share one runtime variable. PoC: 110 vs
  correct 106.
- **`revert(off,len)` drops the payload** (`DataOps.cpp:459-469` ⇒ `assert false`): the
  `mstore(0, selector) revert(0x1c, 4)` custom-error idiom loses its payload — inconsistent
  with the project's own revert-payload model (the oracle diffs payloads).

### H13 ✅ Unaligned `keccak256` constant-length silently truncates
`assembly/DataOps.cpp:341,427` — `len > 32` and not word-aligned hashes only
`floor(len/32)*32` bytes. PoC: `keccak256(0x84, 0x30)` hashes 32 bytes. Kills
`abi.encodePacked(address, bytes32)`-shaped hash idioms with a wrong-but-plausible hash.

### H14 Function-pointer external-call encode/dispatch drift (itxn/) ○
- `FunctionPointerDispatchTypes.cpp:78-125`: external fn-ptr args use a private encoder, not
  the consolidated `InnerCallHandlers::encodeArgToBytes` — negative int128 arg produces a
  32-byte wire value where the callee asserts len==16 (revert); arrays/structs skip ARC4
  encoding entirely. The fourth copy the AbiCodec consolidation missed.
- `FunctionPointerBuilder.cpp:476-497`: dispatch-subroutine definition types
  (`mapDispatchType`) disagree with call-site types for address/enum/array/struct; multi-return
  definitions are `void` (TODO) while call sites expect a tuple — silently dropped results.
- `dispatchName` (364-391) collapses signedness and all non-int types to `_x`, merging distinct
  pointer signatures into one dispatch group typed by whichever was registered first.

### H15 ✅ Contract-dispatch highs (contract/)
- **`super`/`Base.f()` targets carry ABI-entry semantics**: `SuperCallResolution.cpp:188`
  resets `arc4MethodConfig` only *after* `buildFunction` has baked in the not-payable assert,
  entry checks, and wire-return encoding. Verified in TEAL: a payable `g()` calling `A.f()`
  inherits `assert // not payable` and falsely reverts when grouped with a payment.
- **Cross-contract keyed public getters always revert**: callee publishes `m(uint256)uint256`
  while the caller side emits `m()byte[]` (`PublicGetterBuilder.cpp:587-615` + caller in
  itxn/CallResolver) — router `err`. Param-less getters were fixed (night-2); keyed ones were
  not. Related ○: getter biguint keys always publish as `uint256` while the explicit-function
  twin uses declared bits (`PublicGetterBuilder.cpp:594` vs `FunctionBuilder.cpp:471`).
- **Memory-param write-back augmentation misses returns in loops/nested blocks** — both twins:
  `FunctionBuilder.cpp:230-264` (verified: valid Solidity → puya type error) and
  `AWSTBuilder.cpp:757-761` (library path; also misses Switch). A `return;` inside a `for` in a
  mutating function fails to compile (fail-loud, but rejects valid code).

### H16 ✔ Transient sub-64 signed reads come back unextended
`storage/TransientStorage.cpp:174-187` — the uint64 branch of `buildRead` does bare
`btoi(extract(...))`; only the biguint branch sign-extends. The cell convention is 64-bit TC
(`SlotWordCodec.cpp:109-121` sign-extends; the write side truncates). `int32 transient x = -1;`
reads back as 4294967295. int8..int56 affected; int64/int128+ fine.

---

## Part III — Medium

- **M1 ✔ Tuple destructuring applies no per-element coercion**
  (`stmts/SolVariableDeclaration.cpp:377-399`): `makeTupleItem` stamps the *declared* wtype on
  slots holding *RHS* wtypes; no `coerceForAssignment`, no sign-extension (single-decl path has
  both at 126-145). `(int128 a,) = (int8Val,)` binds 0xFF as +255.
- **M2 ✔ Bare `return;` with ≥2 named returns emits `ReturnStatement(nullptr)`**
  (`stmts/SolExpressionStatement.cpp:133-148` — only `size()==1` handled).
- **M3 ✔ Packed slot-handle element *read* double-evaluates the index**
  (`storage/SlotHandleAccess.cpp:154-156` — same `_idx` node in slot math and `packedBEPos`;
  the write path binds a temp for exactly this reason at 176-179).
- **M4 ○ Assignment-as-expression yields stale/sentinel values** when the write is queued
  pending (transient: `SolAssignmentEarlyOuts.cpp:62-67`; slot-handle elem/field writes return
  `makeZero` sentinels, `SolAssignment.cpp:258,338`). `uint a = (t = 5);` reads t's old value.
- **M5 ○ Assignment builds LHS before RHS** (`SolAssignment.cpp:54-55`); solc (both pipelines)
  is RHS-first. `arr[i++] = i;` diverges. Formally unspecified in Solidity, but the
  differential oracle compares against real solc.
- **M6 ○ Yul `if` revert-body detection over-collapses** (`assembly/ControlFlowOps.cpp:29-52`):
  any top-level `revert` in the body replaces the *whole body* with `assert(!cond)` — dropping
  a preceding conditional `leave` (falsely reverts) and payload-building mstores.
- **M7 ○ Slot-0-only legacy memory paths** (mstore8, readMemSlot, storeResultToMemory, keccak
  reads, calldatacopy write, precompile I/O — `assembly/MemoryOps.cpp:583` et al.) bypass
  multi-slot routing: offset ≥4096 panics or reads the wrong slot in `--evm-memory-slots`
  contracts (honk FMP reaches ~18KB).
- **M8 ○ asm `call`/`staticcall` runtime-address path**: `value` silently ignored (no payment);
  `inSize < 4` underflow-panics (plain value-transfer `call(g,to,amt,0,0,0,0)` crashes); output
  copy is ARC4-prefix-shifted vs `returndatasize()` counting the prefix
  (`assembly/PrecompileDispatch.cpp:216-341`). Small **constant** non-precompile addresses fall
  to warn + `success=true` stub (187-194) — the pattern the hard errors were added to kill.
- **M9 ○ ecPairing hard-codes the 2-pair layout, no length assert**
  (`itxn/InnerCallShapes.cpp:416-451`): >2 pairs checks only the first two — a 4-pair Groth16
  verify can accept invalid proofs; <2 pairs runtime-panics. Needs `assert len == 384` minimum.
- **M10 ○ modExp hard-codes 32/32/32 EIP-198 headers, never asserts them; `mod=0` panics
  instead of returning zero** (`assembly/PrecompileHandlers.cpp:229-283`).
- **M11 ○ transient asm `tload/tstore`**: keccak-derived slots (any transient mapping) panic in
  `btoi` (>8 bytes); slots ≥128 overrun the 4096-byte blob (`assembly/SignedOps.cpp:15-54`).
- **M12 ○ Dynamic-offset `calldataload`/`calldatacopy` panic past calldata end** where EVM
  zero-pads (`assembly/DataOps.cpp:24-32`) — the standard tail-word read loop reverts.
- **M13 ○ bytes-local `mcopy` lacks the guarded/truncated write its mstore siblings got;
  blob `mcopy` is copy-forward, not memmove** (`assembly/MemoryOps.cpp:462-544`,
  `StatementOps.cpp:606-672`).
- **M14 ✅ `arc4DefaultEncoding` doesn't bit-pack consecutive `arc4.bool` fields**
  (`sol-types/Arc4Defaults.cpp:124-179` vs `computeEncodedElementSize` which packs 8/byte):
  defaulted `mapping(K=>S)` values with ≥2 leading bools + a dynamic field have head offsets
  that disagree with puya's reader — read-then-modify splices at the wrong position.
- **M15 ○ Internal-call write-back silently dropped** for field paths deeper than 1, non-struct
  roots, and non-VarExpression memory args (`SolInternalCall.cpp:607-665`) — fail-loud policy
  violation; `Lib.mutate(s.inner.arr)` compiles clean and loses the mutation.
- **M16 ○ Self-call resolution matches name+arity only**, ignoring the signature's types
  (`InnerCallHandlers.cpp:545-551`), and passes args unc coerced; plus the
  `handleCallWithEncodeCall` fallback twin mis-encodes returns (bare itob / unpadded biguint)
  and drops extra return values (`InnerCallShapes.cpp:55-115`).
- **M17 ○ `.transfer`/`.send`/ASA amounts truncate uint256→uint64 mod 2^64 silently**
  (`AsaIntrinsics.cpp:46-58`, `InnerCallHandlers.cpp:438,446`): `transfer(100 ether)` sends
  `1e20 mod 2^64` microalgos. Needs a high-bits assert.
- **M18 ○ Overridden base overloads re-emitted as duplicate ABI methods**
  (`ContractBuilder.cpp:574-607` dedup key `name#id` never consults overriddenIds) — currently
  saved by emission order; ordering-dependent landmine (verified: two `f(u256)` methods in
  AWST).
- **M19 ○ `__postInit` is an unauthenticated ABI method** re-supplying ctor args
  (`ApprovalProgramBuilder.cpp:375-417`), and regular methods dispatch while `__ctor_pending`
  — deploy front-run / pre-init window unless tooling always groups atomically. Worth
  `assert !__ctor_pending` on regular routes and creator-only postInit.
- **M20 ○ `.selector` on a ternary evaluates the condition twice**
  (`members/SolSelectorAccess.cpp:96,103`).
- **M21 ○ Sized calldata arrays (`uint[2] calldata`) classified as dynamic pointers in asm**
  (`stmts/SolInlineAssembly.cpp:454-464` — the static ReferenceArray branch is dead code);
  asm reads then follow the offset+length protocol though only the offset local exists.
- **M22 ○ Inline array literals as external-call args hand-encode 32-byte words**
  (`SolExternalCall.cpp:356-378`), bypassing `encodeArgToBytes`: wrong element width for
  narrow types, no length header for dynamic params, zero- instead of sign-extension.
- **M23 ○ External call to a public state-var getter omits param types from the selector**
  (`SolExternalCall.cpp:49-50`) — same family as H15's getter mismatch, different site.
- **M24 ○ `mulmod`/`addmod` evaluate the modulus (and its zero-assert) before x/y**
  (`sol-eb/BuiltinCallables.cpp:87-124`) — revert-payload divergence the oracle can see.
- **M25 ○ `emitOverflowCheck` pre-statement placement**: fixed for uint256 via inline comma,
  uint65..uint255 still take the pre-statement form in the same broken contexts
  (`SolIntegerBuilder.cpp:652-674`).
- **M26 ✔ Cross-file same-name libraries collide on subroutine id**
  (`FunctionIdRegistry.cpp:38-70` — `_sourceFile` is the constant main source; two vendored
  `Math` libraries in different files map to one id; `subMap` keeps one arbitrarily). Free
  functions got AST-id disambiguation (85-96); libraries didn't.
- **M27 ○ `augmentReturns` sibling gaps in AWSTBuilder**: >4KB blob-backed named returns in
  the implicit multi-return tuple use the aggregate name/wtype instead of `__blobagg_off_<id>`
  /uint64 (`AWSTBuilder.cpp:834-850`); mixed named/unnamed fall-through builds a nameless
  VarExpression.

---

## Part IV — Low / design / dup (abridged)

- `stop`/`invalid` in asm don't set `m_haltEmitted` and `stop` ignores `m_frameIsProgram`
  (`StatementOps.cpp:573-584`).
- `shr/sub/eq/lt/...` don't pre-wrap >2^256 operands the way `shl` does
  (`BitwiseShiftOps.cpp:364-398`, `ArithmeticOps.cpp:144-151`).
- Yul switch: uint64 scrutinee vs biguint case literals; string-literal cases
  (`ControlFlowOps.cpp:168-241`).
- Signed asm ops / sar / runtime signextend duplicate pure subtrees 4-6× — cost, not
  semantics; multiplies `__storage_read` calls (`SignedOps.cpp`, `BitwiseShiftOps.cpp:462`).
- Dead code: keccak sub-32 hard-error + calldata-pattern loop unreachable behind the
  early-return warn path (`DataOps.cpp:350-395`).
- `resolveConstantOffset` folds constant `sub` with uint64 wrap (`AssemblyBuilder.cpp:605`).
- Unimplemented precompiles via `.call` (vs `.staticcall`) warn then fall through to an inner
  app call to app-id 2..10 (`InnerCallShapes.cpp:453-457`); no handler exists for 0x0a at all.
- `{gas: expr}` clause silently discarded without building (side effects dropped)
  (`SolFunctionCall.cpp:28-46`).
- Low-level call success is compile-time `true` (AVM inner-txn failure aborts the caller) —
  known semantics, but absent from EVM_DIVERGENCE.md; document it.
- `fundCreatedApp` reads live `itxn CreatedApplicationID` against the capture discipline
  (`InnerCallHandlers.cpp:801`).
- `fallback(bytes) returns(bytes)` return data discarded (`SelectorRouter.cpp:34`).
- PostInitTriggers indirect detection is one call level deep in the unsafe direction
  (`PostInitTriggers.cpp:103-134`) — missed triggers break deploys at runtime.
- ModifierBodyInliner return-var-assignment hoist can reorder across the modifier gate —
  dormant today (`ModifierBodyInliner.cpp:411-425`).
- Self-recursion ARC4Encode fix-up indexes args positionally wrong and is empirically inert
  (`FunctionBuilder.cpp:912-928`).
- `m_ensureBudget` keyed by bare name — collides across overloads (`FunctionBuilder.cpp:934`).
- Storage-ref locals bound from calls use bare `decl.name()` — ALWAYS-MANGLE violation;
  shadowing aliases (`SolVariableDeclaration.cpp:209-226`, `SolIdentifier.cpp:104-115`).
- Tuple-snapshot temps keyed by source line collide on nested destructure
  (`SolAssignmentTuple.cpp:175`).
- `visitContinue` splices `forLoopPost`/`doWhileCondBreak` shared nodes without the deep-clone
  `visitPlaceholder` does — SE-id aliasing across tree positions (`SolBlock.cpp:97-115`).
- Unresolvable mapping storage-ref args fall back to constant prefix `"map"` — two such args
  alias one namespace (`SolInternalCall.cpp:248`).
- `.length` on box tuple: same raw `box_get` IntrinsicCall in two TupleItems — the very shape
  SolAddressProperty documents as a puya miscompile and temps around
  (`SolLengthAccess.cpp:255-260`); reconcile which belief is right.
- Slot-ref outer-dim access over dynamic inner arrays silently returns the base slot,
  dropping the index (`SolIndexAccess.cpp:175-176`).
- `layoutFor` clamps stride at 4096 slots silently (`SlotHandleAccess.cpp:60`);
  `shouldUseBoxStorage` wraps at 2^32 bytes (`StorageMapper.cpp:288`); `TypeMapper::map`
  truncates u256 array lengths to int64 (`TypeMapper.cpp:72`).
- ReassignWalker misses tuple-assignment LHS and modifier bodies (`AWSTBuilder.cpp:119-142`).
- Modifier `mappingKeyList` narrower than body registration (bytes-keyed struct returns)
  (`AWSTBuilder.cpp:537-545` vs 586-594).
- `natSpecTagValue` prefix-matches tags — needs a delimiter check (`NatSpecTags.h:15`).
- SetGas/SetValue/Declaration routed to SolBytesConcat; unmatched member access lowers to a
  typed zero with only a warning (`SolExpressionFactory.cpp:385`,
  `SolExpressionDispatch.cpp:143`).
- Fallback comparison arm lowers signed ordering as unsigned instead of failing loud
  (`BinaryOpBuilder.cpp:174-191`).
- Dup worth consolidating: bytesN constant-padding ×2 (`BinaryOpBuilder`/`SolFixedBytesBuilder`);
  signed element-widening loop ×3 (two in TypeCoercion lack pinning — one is a live
  double-eval, see H11 tail; `Arc4ArrayWidening` has the disciplined copy); StorageAlias
  classification lambda ×3 (already drifted on the drain fix — see Critical 3); the
  pin-encode-box_replace block ×3 on ad-hoc `static int` counters; ARC4
  widening/narrowing/encode ladder duplicated in SolAssignment vs SolAssignmentTuple (tuple
  copy lacks the bytes→ARC4-byte-array fix).

---

## Part V — The systemic reads

**T1 — Pending-statement drain discipline is THE seam this round** (Criticals 3-4, H1-H5, M4,
M15-adjacent; ~15 findings). Every statement-level handler must drain; today each re-implements
the choice. Two concrete moves: (a) a debug-mode assert that the pre/post buffers are empty at
statement boundaries (translate-statement wrapper), which turns every future hole into a loud
compile-time failure on the test corpus; (b) fold drain-and-emit into a single helper that
`toAwst` implementations *cannot* skip (the OperandPlan primitives from review-1 item 7 are the
expression-side half of this; the statement side never got its equivalent).

**T2 — EvalOnce at builder entry, not call sites** (H11 + M3, M20, M24 tail; ~12 findings).
The sol-eb builders receive raw operand trees and reference them 2-12×. The signed binary path
pins; nothing else does. Pin once in `builderForInstance`/`unary_op` dispatch (or assert
"operands must be trivially-duplicable" there) instead of chasing call sites forever.

**T3 — New class N1: stale compile-time caches** (Critical 2, M6-adjacent, the
`m_lastMstoreValue` folds). The asm layer's constant caches have no kill/scope semantics.
Restrict to single-assignment locals + straight-line memory constants, or drop the folds.

**T4 — C3 twin paths keep drifting where one twin got a fix** (Critical 4, H8-packed, H9, H14,
H15-getters, M22-M23, M25). The postInit/base-ctor, compound-assign, fn-pointer, and getter
seams each have a fixed twin and an unfixed one. When landing any fix, grep for the twin — the
bug ledger now shows five instances where the twin was missed.

**T5 — One mutation/return walker.** `ParamMutationDetector` (assignment-only),
`augmentReturns` (IfElse-only), `augmentMethodForMutatedMemoryParams` (IfElse-only), and
`forEachReturnStatement` (Block+WhileLoop) are four partial tree-walks over the same two
questions ("is this param mutated", "where are the returns"). Consolidate on one visitor each;
H6 and both H15/M27 walk gaps fall out for free.

**Suggested attack order:** Critical 1 (one-line guard + real fix), Critical 2 (single-assignment
restriction), Criticals 3-4 + T1 assert (one cluster), then H6-H8 (storage-semantics trio),
H9-H11 (arith/encode trio), H15 (dispatch trio), with T2's builder-entry pinning as the
umbrella refactor once H11's instances are individually guarded by tests.
