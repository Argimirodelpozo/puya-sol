# solc-reuse opportunities

Places where puya-sol re-derives a value / size / type that the vendored solc
(`libsolidity`) already computes exactly — and has gotten subtly wrong. Leaning on
solc's computed facts removes hand-rolled code *and* whole bug classes.

**The throughline (proven by the differential fuzzer):** nearly every value-level
divergence found this cycle — wide-array `.length`, `int8(-1)` non-canonical,
signed sub-word compare/equality — is puya recomputing something solc already has.
The fuzzer is effectively a detector for "haven't deferred to solc here yet."

**UPDATE 2026-06-22 — the canonicalization sub-throughline (now the dominant one).** An overnight
fuzz+fix run shipped **7 zero-reg fixes (v427–v433), and 6 of them are the SAME bug**: a sub-256
unsigned biguint left NON-CANONICAL (value exceeding 2^N, or a signed value not sign-extended) by a
width-growing op, which the return/store path masks but a downstream CHECKED consumer or comparison does
NOT — so it false-reverts or mis-compares. Sites patched ONE AT A TIME as the fuzzer found them:
left-shift (v427), bitwise-NOT (v428), signed→unsigned cast (v429), unchecked sub+exp (v430), complex
signed-mul operand (v431), `-type(intN).min` (v432). Each fix is "mask/sign-extend to the type width at
this site." This is precisely the failure mode **opportunity F** (centralize canonicalization per-WType)
exists to kill: today every CONSUMER must remember to canonicalize; the fuzzer just enumerates the ones
that forgot. The business case for F is now proven, not hypothetical. (v433, short-circuit `&&`/`||`
RHS scoping, was a separate codegen-scoping fix — not a solc-reuse item.)

---

## A. Use `ConstantEvaluator` / `literalValue()` for ALL constant subexpressions
**Status: highest value. Proven by the `int8(-1) == int8(-1)` bug (fixed 2026-06-21).**

> **Step 1 DONE (v420, 0bec5aed44):** `type(intN).min/max` now uses solc's `IntegerType::min()/max()`
> (256-bit TC u256) via a new shared `TypeCoercion::canonicalIntConstant(tcValue, bits)` — the single
> "solc value -> canonical int constant" rule. Deleted ~40 lines of hand-rolled TC math in
> SolMetaTypeAccess + the "lockstep with SolLiteral" coupling. Zero-reg, guard test_type_minmax_canonical.
> **Step 2 DONE (v421, 4c7f7032e3):** (a) SolLiteral's signed-small `%2^64` wrap branch turned out to be
> DEAD code (needed m_solType to be RationalNumberType AND IntegerType at once) — removed, verified dead by
> fuzz. (b) SolLiteral + tryConstantFold share the same magnitude rule but are width-less rationals, so they
> do NOT fit `canonicalIntConstant` (which needs a fixed width); extracted a separate small
> `TypeCoercion::rationalIntConstant` and routed both through it. Two constant-emission shapes now exist:
> width-based (`canonicalIntConstant`, for type(T).min/max + intN casts) and magnitude-based
> (`rationalIntConstant`, for rationals).
> **Step 3 (the const-fold gap) DONE — but it DID NOT EXIST (v422 guard, no compiler change):** investigated
> with the differential fuzzer + harness. `type(uint64).max**2` does NOT fold-and-widen on EVM — its type is
> `uint64`, so `(2^64-1)^2` overflows in checked context and reverts on EVM AND AVM identically. The old
> "gap" note was a misread: the fuzzer's "no divergence" for it was a BOTH-revert, not a value match.
> Constants that fit ARE folded correctly (`10**77`, `1<<200`, `2^255`, `3*2^200`), unchecked ones wrap in
> their operand width on both — all match EVM. So `ConstantEvaluator::evaluate()` integration buys nothing.
> Locked by guard `test_const_fold_arbitrary_precision`. **Opportunity A is now COMPLETE** (steps 1-2 were
> consolidation; step 3 was a non-issue). NB: SolLiteral was already canonical; the `int8(-1)` bug was the
> explicit-cast path only (SolTypeConversion) — A was consolidation, not bug-fixing.

Today `ConstantEvaluator` is only used in `SolInlineAssembly.cpp`. The high-level path
hand-rolls constants:
- `type(intN).min` is computed by hand in `SolMetaTypeAccess.cpp`.
- `SolLiteral.cpp` has a "wrap signed-small literals to 64-bit TC so comparisons line
  up" hack (line ~42-53).
- `int8(-1)` reached equality as a non-canonical `255` (cast masked, didn't sign-extend).

If constant subexpressions were folded via solc and **emitted in canonical form**:
- delete the hand-rolled `type(T).min/max` lowering + the SolLiteral wrap hack,
- close the `type(uint64).max ** 2` const-fold gap (currently a runtime revert vs EVM's
  arbitrary-precision fold),
- erase the non-canonical-constant bug class at the source.

Where: `SolLiteral.cpp`, `SolMetaTypeAccess.cpp`, `SolTypeConversion.cpp` (constant cast
fast-path). Reuse `Type::literalValue()` / `ConstantEvaluator::evaluate()`.

## B. Reuse solc's storage layout (`--storage-layout`: slot + offset + packed)
solc assigns every state var an exact `(slot, byteOffset, inStorage)` layout. The
EVM-faithful assembly `sload`/`sstore` path (`StorageDispatch`) recomputes slot math and
has had aliasing bugs (mod-256 computed-slot collisions; see silent-stub-sweep notes).
Adopt solc's computed slots as the source of truth for the asm-storage path instead of
parallel math. Caveat: puya's high-level state uses boxes, not EVM slots — this applies
to the assembly/EVM-faithful path, not box storage.

## C. `Type::storageBytes()` / `calldataEncodedSize(false)` for element & field sizes
**Investigated 2026-06-21: NOT viable as a swap, and no latent bug to catch.**
`computeEncodedElementSize` (`Arc4Defaults.cpp`) is **WType-based** (24 call sites, many of which
only have an ARC4 WType, not a Solidity `Type`), so solc's `calldataEncodedSize` (a Solidity-`Type`
method) can't replace it uniformly. And the sizes are **context-dependent**: BOX storage is packed
ARC4 (uint128 = 16B → `mapSolTypeToARC4`) while the MEMORY BLOB is 32-byte words (uint128 → 32B via
`map()`), so there's no single right answer per type. `bool` (puya 8B) and `address` (puya 32B account)
also genuinely differ from solc's ABI (1B / 20B) — by design. For integers the hand-rolled switch
already agrees with solc (`ARC4UIntN(n) → n/8`). The wide-array bug was a call-site input error (a
width-erased `map()` feeding the box path), already fixed; an audit of the other sites found them
correct-for-their-context (blob sites correctly use 32). Box + memory sub-word aggregates fuzzed CLEAN
vs live EVM; guard `memory_subword_aggregate` added for the memory path (was uncovered). Same shape as
the commonType finding: puya's type model (WType, box-vs-blob, bool/address widths) diverges from solc
at exactly these size seams — which is *why* puya has its own abstraction.

## D. `commonType` + `mobileType()` + the implicit-conversion lattice  ← TACKLING NEXT
solc annotates every `BinaryOperation` with `annotation().commonType` (the type both
operands are implicitly converted to before the op) and every expression with its exact
`annotation().type` / `mobileType()`. `TypeCoercion` reimplements numeric promotion and
literal-typing. Driving operand conversions from solc's `commonType` keeps puya consistent
with the exact rules the fuzzer keeps probing, and drops the parallel promotion logic.
Promising for the implicit operand casts inside binary ops / comparisons — where the
width/sign bugs live.

Where: `SolBinaryOperation.cpp`, `TypeCoercion.cpp` (implicitNumericCast / promotion).

**DONE 2026-06-22 (comparisons), v424 (aa1f493e57).** The earlier "net-additive" read missed the key move:
make the `commonType` coercion ALSO canonicalize (sign-extend from each operand's own width), and it
*replaces* `compare()`'s per-operand machinery instead of duplicating it. SolBinaryOperation coerces both
integer comparison operands to the op's `commonType` via one shared `TypeCoercion::coerceToCommonInt`;
`compare()` then gets uniform same-width canonical operands and **deleted** both `narrowConstIfNegative`
(the fragile biguint-const mod-2^64 hack) and the inline ordering+equality sign-extension — it collapses
to resolve -> promoteToBigUInt -> sign-bit XOR. The literal-operand objection dissolves because we coerce
TO `commonType` (always the integer common type) rather than guarding on the operand's own
`RationalNumberType`. Roughly LOC-neutral but structurally a real win: the scattered per-operand fix-ups
become one solc-`commonType`-driven point. Verified zero-reg + 191-call mixed-width fuzz + signed guards.
**Residual DONE 2026-06-22 (v434, 7615a14597) — and it was an ACTIVE bug, not latent.** The arith/bitwise
path (`SolBinaryOperation.cpp` hasBinOp) reinterpreted the LEFT operand to `commonType` without value
conversion AND never touched the right. Driving BOTH operands through `coerceToCommonInt` (mirroring the
v424 comparison path) fixed a real divergence: mixed-width signed BITWISE (`int128(-1) & int16(-32768)`)
ANDed the raw 16-bit operand instead of the sign-extended commonType value — wrong in both
narrower-left and narrower-right positions. Shifts are excluded (right = shift amount, kept in its own
type); unsigned zero-extends; non-integer commonType keeps the bare reinterpret. Guard
test_mixed_width_signed_bitwise. Lesson: the "latent" reads in this doc are worth fuzzing — the
canonicalization-at-source did NOT cover the mixed-width bitwise consumer.

## E. `FunctionType::externalSignature()` for canonical signature strings
**NOT VIABLE — investigated 2026-06-22.** The original pitch (reuse externalSignature for the
"canonical-param-type-name string") was wrong: puya-sol has NO EVM-ABI signature anywhere, so there is
nothing for externalSignature to feed. Every signature string is built from the ARC4 type-name rules,
which diverge from EVM ABI on TWO axes (TypeCoercion::intSelectorName / wtypeToABIName):
1. **signedness is always dropped** — `int128` is named `uint128` (ARC4 has no signed type);
2. **sub-64 widths collapse to `uint64`** — `int16` / `uint8` / `uint64` all → `uint64`.
externalSignature() emits the exact Solidity types (`int128`, `int16`, `uint8`), so it can produce
NEITHER. Selectors AND custom errors hash the ARC4 string with `sha512_256(sig)[:4]` (MethodConstant),
not EVM keccak; the only EVM-literal holdouts (`Error(string)` / `Panic(uint256)`) are HARDCODED magic
constants (0x08c379a0 / 0x4e487b71), not constructed. Same conclusion as C/F: puya's ARC4 ABI model is
deliberately distinct from solc's, which is exactly why these "reuse solc" swaps don't land. The
selector-width logic is already correct (sol-eb-audit) and has no solc equivalent to defer to.

## F. Model `Type::cleanupNeededForOp` — the principled "centralize canonicalization"  ← GOAL SUBSTANTIALLY MET (closed 2026-06-22)

**CLOSED 2026-06-22 (audit + decision).** F's GOAL — kill the "every consumer must remember to
canonicalize" bug class — is substantially achieved, NOT via a per-WType `canonicalize()` but via ~5
shared canonicalizers that the 38 call-sites (across 17 files) already route through:
`signExtendToUint256/64`, `signExtendSignedElement`, `signExtendSignedWiden`, `maskUnsignedToWidth` (new,
the producing ops), `coerceToCommonInt` (operand conversion — comparisons v424 + arith/bitwise v434).
This session finished the PRODUCER side: every width-growing op (shift, `~`, signed→unsigned cast,
unchecked wrap) now canonicalizes at the source, so the value is canonical before any consumer sees it.

**Why the literal "attach `canonicalize()` to each WType" is NOT built (and shouldn't be):** puya's
WType deliberately carries NO signedness (intN → `ARC4UIntN(n)` + an alias string), so a per-WType
canonicalize() cannot decide sign-extend-vs-not without the Solidity type. And the sub-64 canonical form
is CONTEXT-DEPENDENT (locals minimal, ABI params pre-extended — see [[int24-subword-codec]]), which is
exactly why `signExtendSignedElement` deliberately skips ≤64-bit. A blanket auto-canonicalize would
re-introduce the double-extension bugs the existing helpers avoid. So the realistic ceiling for F is "one
named helper per canonicalization shape, called explicitly by sites that have the Solidity type" — which
is what exists. The full consumer-flip (make decode/read producers canonical, delete the defensive
consumer extends) is high-risk for diminishing value now the bug class is closed. Decision: leave it.

### (historical pitch below)
## (was) F. Model `Type::cleanupNeededForOp`
solc attaches, per type and per op, whether a value needs cleanup before use, decided
once. That's the disciplined form of collapsing puya's ~54 scattered `signExtend*` call
sites (across 19 files): attach a `canonicalize()` to each WType rather than remembering
it at every consumer.

**Evidence (2026-06-22): 6 fixes in one overnight run were all "forgot to canonicalize at this site."**
v427 left-shift, v428 bitwise-NOT, v429 signed→unsigned cast, v430 unchecked sub+exp, v431 complex
signed-mul operand, v432 `-type(intN).min` — each a `% 2^N` mask or sign-extend at the producing op,
because a downstream checked consumer / `<= max` compare saw the non-canonical value. The fuzzer will
keep finding the next forgotten site until canonicalization is correct-by-construction. The model:
- A WTYPE INVARIANT: a uint/intN value is always stored masked-to-2^N (unsigned) or sign-extended-to-
  256-bit (signed sub-256). Producers that can violate it (shift-left, `~`, sign-ext cast, unchecked
  wrap) canonicalize; consumers can then ASSUME canonical and drop their defensive masks.
- equivalently `cleanupNeededForOp(type, op)` decided once per (type, op) instead of remembered at 54
  consumer sites.

**Incremental path toward F (do these first):**
1. Extract one `TypeCoercion::maskUnsignedToWidth(v, bits)` (and pair with the existing
   `signExtendToUint64/256`) and route the ~5 inlined v427–v432 masks through it — pure consolidation,
   names the invariant, lowers the cost of the full refactor.
2. Audit the ~54 `signExtend*`/mask sites: classify each as PRODUCER-side (keep, it establishes the
   invariant) vs CONSUMER-side (candidate to delete once producers are canonical).
3. Attach `canonicalize()` to the WType (or a `(type,op)->bool` table) and flip consumers to assume.

## G. `type(I).interfaceId`, C3 linearization, overload resolution
**ALREADY DONE — investigated 2026-06-22. Nothing to do; "likely already partly used" was an
understatement — it's fully reused where reusable.** All three parts:
1. **C3 linearization** — puya already drives super/virtual dispatch off solc's
   `annotation().linearizedBaseContracts` at 10+ sites (AWSTBuilder, ContractBuilder, SuperCallResolution,
   ApprovalProgramBuilder, PostInitTriggers). No parallel MRO is reimplemented.
2. **Overload / reference resolution** — `referencedDeclaration` is used across ~12 files (AWSTBuilder,
   CallResolver, SolExpressionFactory, …); puya never re-resolves overloads itself. interfaceId even reuses
   solc's `interfaceFunctionList(false)` for the own-functions enumeration.
3. **interfaceId** — already implemented (SolMetaTypeAccess): XOR over the functions, but with ARC-4
   `sha512_256` selectors, NOT solc's EVM keccak XOR (same ARC4-vs-EVM divergence as E — solc's interfaceId
   VALUE can't be reused, only its function list, which it is). The on-chain `supportsInterface` XOR stays
   self-consistent. (Aside: `externalSignature()` survives as a narrow FALLBACK there for non-FunctionDefinition
   entries — the one place E's helper is actually touched.)
Net: the LANGUAGE-level facts (MRO, reference resolution, function lists) are reused from solc; only the
ABI-level selector VALUE is ARC4-specific (as it must be). G is closed.

---

### Priority
1. ~~**A** (ConstantEvaluator / canonical constants)~~ — **DONE** (v420-v422): type(T).min/max + SolLiteral
   dead-branch + shared canonicalIntConstant/rationalIntConstant; const-fold gap debunked (never existed).
2. ~~**D** (commonType for comparisons + arith)~~ — **DONE** v424 (comparisons) + v434 (arith/bitwise both
   operands): coerceToCommonInt drives operand conversion off solc commonType; deleted narrowConstIfNegative
   + inline compare() canonicalization; the arith-path residual turned out to be an active mixed-width
   signed-bitwise bug, now fixed.
3. ~~**F** (centralize canonicalization)~~ — **GOAL SUBSTANTIALLY MET / CLOSED** (2026-06-22). The bug
   class is killed via ~5 shared canonicalizers; this session finished the producer side: ~~(1) extract
   `maskUnsignedToWidth`~~ DONE (8a4b6d4284); ~~(2) D-residual arith both-operand coercion~~ DONE (v434,
   7615a14597, an active bug). The literal per-WType `canonicalize()` is NOT built — puya's WType has no
   sign info and sub-64 canonical form is context-dependent; the full consumer-flip is high-risk for
   diminishing value. See the F section for the close-out rationale.
4. **C** (size), **B** (storage layout), **E** (externalSignature) — all NOT VIABLE / off: each re-derives
   something solc has, but puya's ARC4/box/WType model is deliberately distinct (sub-word widths, box-vs-blob,
   bool/address widths, signed→uintN selector collapse, 4 KB boxes not slots), so there is no solc fact to
   defer to. The pattern: "reuse solc" lands for VALUES (constants, commonType operand conversion) but not
   for puya's ABI/storage ABSTRACTIONS.
5. ~~**G** (interfaceId / C3 / overload resolution)~~ — **ALREADY DONE.** C3 (`linearizedBaseContracts`,
   10+ sites), reference/overload resolution (`referencedDeclaration`, ~12 files), and interfaceId's
   function-list enumeration (`interfaceFunctionList`) are all already reused from solc; only the
   ARC4 selector VALUE is puya-specific (as it must be). Nothing to reimplement.

---

### Bottom line (2026-06-22): the solc-reuse backlog is worked through.
- **Reuse LANDS for solc's VALUES / language-level facts** — and is now done: A (constants), D
  (commonType operand conversion → caught a real mixed-width bitwise bug), G (MRO / reference resolution /
  function lists, already reused), F's goal (canonicalization via shared helpers).
- **Reuse does NOT land for puya's ABI / storage / WType ABSTRACTIONS** — B (4 KB boxes ≠ EVM slots),
  C (box-vs-blob packed sizes), E (ARC4 sha512_256 selectors, signed→uintN, sub-64→uint64 ≠ EVM ABI).
  These are deliberately distinct; there is no solc fact to defer to. Confirmed, not skipped.
The one open item anywhere is the documented backend-DCE soundness edge (degenerate; left by decision).
