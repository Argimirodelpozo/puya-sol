# solc-reuse opportunities

Places where puya-sol re-derives a value / size / type that the vendored solc
(`libsolidity`) already computes exactly — and has gotten subtly wrong. Leaning on
solc's computed facts removes hand-rolled code *and* whole bug classes.

**The throughline (proven by the differential fuzzer):** nearly every value-level
divergence found this cycle — wide-array `.length`, `int8(-1)` non-canonical,
signed sub-word compare/equality — is puya recomputing something solc already has.
The fuzzer is effectively a detector for "haven't deferred to solc here yet."

---

## A. Use `ConstantEvaluator` / `literalValue()` for ALL constant subexpressions
**Status: highest value. Proven by the `int8(-1) == int8(-1)` bug (fixed 2026-06-21).**

> **Step 1 DONE (v420, 0bec5aed44):** `type(intN).min/max` now uses solc's `IntegerType::min()/max()`
> (256-bit TC u256) via a new shared `TypeCoercion::canonicalIntConstant(tcValue, bits)` — the single
> "solc value -> canonical int constant" rule. Deleted ~40 lines of hand-rolled TC math in
> SolMetaTypeAccess + the "lockstep with SolLiteral" coupling. Zero-reg, guard test_type_minmax_canonical.
> **Next:** route SolLiteral's signed-small `%2^64` wrap and `SolBinaryOperation::tryConstantFold` through
> the same `canonicalIntConstant` helper (collapses the 3rd/2nd hand-rolled sites). Then consider
> `ConstantEvaluator::evaluate()` for non-literal constant subexpressions (closes the const-fold gap).
> NB: SolLiteral already emits canonical signed-small literals (`val % 2^64`); the `int8(-1)` bug was
> the explicit-cast path only (fixed in SolTypeConversion), so A is now consolidation, not bug-fixing.

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
The wide-array `.length` bug (fixed 2026-06-21) was `computeEncodedElementSize`
(`Arc4Defaults.cpp`) — a hand-maintained `switch` over WTypes — returning 32 for
`arc4.uint128`. solc's `type->calldataEncodedSize(false)` gives the unpadded ABI size
(uint128→16, uint8→1, int16→2, bytes32→32) = the ARC4 packed size for value types.
Replace the switch's value-type arms. Caveat: `address` (32-byte account) and `bool`
(8/byte ARC4 packing) differ from solc, so keep those special-cased.

## D. `commonType` + `mobileType()` + the implicit-conversion lattice  ← TACKLING NEXT
solc annotates every `BinaryOperation` with `annotation().commonType` (the type both
operands are implicitly converted to before the op) and every expression with its exact
`annotation().type` / `mobileType()`. `TypeCoercion` reimplements numeric promotion and
literal-typing. Driving operand conversions from solc's `commonType` keeps puya consistent
with the exact rules the fuzzer keeps probing, and drops the parallel promotion logic.
Promising for the implicit operand casts inside binary ops / comparisons — where the
width/sign bugs live.

Where: `SolBinaryOperation.cpp`, `TypeCoercion.cpp` (implicitNumericCast / promotion).

**Investigated 2026-06-21 (comparisons):** net-additive, NOT a reduction. `compare()` has one
caller (`SolBinaryOperation.cpp:145`) and comparisons don't use `commonType` today — but its
per-operand promotion already handles mixed widths correctly, so coercing to `commonType` first
*duplicates* it rather than replacing it. The canonicalization (cast/equality sign-extend) must stay
regardless (commonType only handles cross-width; same-width non-canonical operands still need it).
`narrowConstIfNegative` (~25 lines) can't be removed because literal operands (`x == -128`) are
`RationalNumberType`, bypass an integer-typed coercion guard, and still reach `compare()` mismatched.
**Residual real win:** the *arithmetic* path (`SolBinaryOperation.cpp:170-178`) uses
`builderForInstance(commonType, _left)` which *reinterprets* the operand without converting the value
— a latent signed-widening gap (a non-canonical operand in `int8 + int16` zero-extends the sign).
Making it *coerce* via `signExtendSignedWiden` would be correct-by-construction. (Today's cast/equality
fixes plug it at the source, so it's latent, not active.)

## E. `FunctionType::externalSignature()` for canonical signature strings
Method *selectors* are ARC4 (different scheme), but the canonical-param-type-name string
construction — used for error/event encoding and the `intSelectorName`/
`arc4EncodeArgsAtParamTypes` width logic that had bugs (see sol-eb-audit selector-width) —
is something solc already builds correctly via `externalSignature()`.

## F. Model `Type::cleanupNeededForOp` — the principled "centralize canonicalization"
solc attaches, per type and per op, whether a value needs cleanup before use, decided
once. That's the disciplined form of collapsing puya's ~54 scattered `signExtend*` call
sites (across 19 files): attach a `canonicalize()` to each WType rather than remembering
it at every consumer. This session alone needed canonicalization added at the cast,
ordering-compare, and equality-compare sites — three places for one invariant.

## G. `type(I).interfaceId`, C3 linearization, overload resolution
solc computes interface IDs (XOR of selectors), `linearizedBaseContracts` (C3 MRO for
super/virtual dispatch), and overload resolution (`referencedDeclaration`). Reuse rather
than reimplement super/interface dispatch (likely already partly used via
`referencedDeclaration`).

---

### Priority
1. **A** (ConstantEvaluator) — cheap, deletes hand-rolled code, retires a live bug class.
2. **D** (commonType for casts) — promising for the operand-conversion width/sign bugs.
3. **C** (size from solc) and **B** (storage layout from solc) — next tier.
4. **E / F / G** — larger or partial-reuse refactors.
