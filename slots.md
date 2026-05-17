# Slot-Based Storage Architecture — Design Notes

Plan for migrating puya-sol's storage model from the current
single-sha256-over-concat scheme to a Solidity-style per-layer
hashing scheme with explicit slot numbers.

## Why migrate

Today: `m[i][k]` for `mapping(K=>V)[N] m` produces box key
`prefix("m") + sha256(concat(itob(i), pad(k, 32)))` — ONE hash over
the concat of every layer's bytes.

Solidity: `arr[i][k]` produces slot `keccak256(h(k) . (p + i))` —
a recursive PER-LAYER hash chain.

The composite-single-hash model can't be incrementally extended.
Storage pointer aliases that name a "partial path" (e.g. `A(m[1])`
passing `m[1]` as a storage-ref arg) bury their key parts inside a
single sha256 call. Continuing the chain from an alias requires
unwrapping the sha256 to extract the inner concat parts (the
"alias-prepend" hack in `SolIndexAccessHandlers.cpp`). Per-layer
hashing makes aliases trivial: the alias IS a slot pointer (one
opaque bytes value), and any further `[k]` / `[i]` operation just
continues the chain.

## Solidity's per-layer rules

For a state variable allocated to slot `p`:

| Layer | Slot transform |
|---|---|
| Mapping `m[k]` | `keccak256(h(k) . parent_slot)` |
| Static array `arr[i]` (1-slot element) | `parent_slot + i` |
| Static array `arr[i]` (N-slot element) | `parent_slot + i * N` |
| Dynamic array body | `keccak256(parent_slot)` once; elements at `that + i * size` |
| Struct field `.x` | `parent_slot + field_offset` |

Key encoding `h(k)`:
- Value types (uint, address, bool, enum, fixed bytes): 32-byte
  big-endian padded.
- `string` / `bytes` keys: raw bytes, no padding.

## Target model for puya-sol

Hash function stays **sha256** (unchanged — we never had keccak256
parity). Only the LAYERING changes.

### Slot allocation

State variables get an integer slot index allocated at contract
translation time. Mirrors Solidity:

- Top-level vars in declaration order across the linearized
  inheritance chain (most-base first).
- Constants and immutables do NOT take slots.

Lives in `StorageMapper` (or a new `SlotAllocator` it owns). Each
`solidity::frontend::VariableDeclaration*` gets a stable
`uint64 slot` registered at compile time.

### Slot pointer representation

A "slot pointer" is a `bytes` value flowing through the layer
transforms. Two representation choices:

- **8-byte `itob(slot)`** — slot numbers are tiny (<<2^64). Short
  box-key prefix. Recommended — Algorand storage doesn't need
  byte-for-byte EVM compatibility.
- 32-byte pad-to-match — bigger, only useful for byte-level EVM
  layout matching, which we don't pursue.

The pointer starts as 8 bytes (the slot's `itob`), becomes 32 bytes
after the first mapping-layer sha256, stays 32 bytes thereafter
(further sha256s preserve width; linear adds preserve width via
biguint arithmetic).

### Per-layer derivation in puya-sol terms

The "current slot pointer" walks one transform per layer:

| Layer | AWST operation | Output bytes width |
|---|---|---|
| Mapping `m[k]` | `sha256(h(k) ++ parent)` | 32 |
| Static array `arr[i]` of non-flat element (e.g. `mapping[N]`) | `biguint_add(parent, itob(i * elem_slots))` | same as parent |
| Static array `arr[i]` of flat T | NONE — whole `arr` is one box; index becomes `IndexExpression(read, i)` on the value | n/a (no derivation) |
| Dynamic array `T[]` access | NONE — whole array is one box; index becomes `IndexExpression(read, i)` on the value | n/a |
| Struct field `.x` (value-type field) | NONE — whole struct is one box; field becomes `FieldExpression(read, "x")` on the value | n/a |

Key encoding `h(k)` matches Solidity:
- Value types: 32-byte right-padded big-endian (same as today's
  biguint encoding).
- `string`/`bytes`: raw bytes, no padding.

### Box value at the slot pointer

The slot pointer becomes the box key. The box stores **one
ARC4-encoded value**:

- Scalar (`uint`, `address`, `bool`, `bytes32`): direct.
- Fixed struct: ARC4 struct as one bytes blob. (Solidity-faithful
  alternative: one box per field at `slot + field_offset` — closer
  to Solidity, enables partial deletes, but multiplies box ref
  count. Recommended: per-blob first; reconsider if a test fixture
  needs per-field semantics.)
- **Dynamic array `T[]`: one box per whole array (current AVM
  model — KEEP).** Solidity stores length at slot + elements at
  `keccak256(slot) + i*size` (one slot per element). We deliberately
  diverge here: AVM's 8-box-ref-per-txn budget would break the
  moment a contract reads >8 elements; the single-box-with-ARC4-
  encoded-array model gives O(1) box refs for whole-array reads at
  the cost of O(N) bytes per push (capped by AVM box-size + concat
  4 KB caps).
- **Static array `T[N]` of value-type T: one box per whole array
  (current AVM model — KEEP)** for the same reason.
- Mapping: no box at the mapping's own slot (Solidity stores
  nothing there either — the slot is a "placeholder").

### Hybrid box-layout policy

We adopt Solidity's per-layer **hashing for derivation** but reject
Solidity's per-element **box layout** for arrays. The split:

| Layer | Derivation | Box count |
|---|---|---|
| Mapping `m[k]` | `sha256(h(k) ++ parent_slot)` per-layer hash | 1 box per (mapping, k) tuple |
| Array of flat T `arr[i]` | None — whole `arr` lives in one box | 1 box per whole array |
| Array of non-flat element `arr[i]` (e.g. `mapping[N]`) | `biguint_add(parent_slot, itob(i * 1))` linear offset | 1 box per (i, k) tuple |
| Struct field `.x` | None — whole struct lives in one box | 1 box per struct |
| Dynamic array `T[]` | None — whole array lives in one box | 1 box per whole array |

**Result**: same box count as today's model. Only the hashing
strategy changes, not the storage layout. The migration's value is
architectural — storage-pointer aliases become trivial — not
runtime.

### Per-access cost comparison (AVM opcodes)

| Pattern | Today (composite) | New (per-layer) |
|---|---|---|
| `m[k]` 1 mapping level | 1 × sha256 + concat ≈ 100 op | 1 × sha256 ≈ 35 op |
| `m[k1][k2]` 2 mapping levels | 1 × sha256 + 2× concat ≈ 130 op | 2 × sha256 ≈ 70 op |
| `m[k1][k2][k3]` 3 mapping levels | 1 × sha256 + 3× concat ≈ 160 op | 3 × sha256 ≈ 105 op |
| `arr[i][k]` array-of-mapping | 1 × sha256 + 2× concat ≈ 130 op | 1 × sha256 + 1 × biguint_add ≈ 50 op |

For 1–2 layer accesses (the common case), per-layer is slightly
CHEAPER than today's composite (no concat overhead). For 3+ layers
of nested mapping, per-layer pays ~35 extra opcodes per level.

### Future hashing optimizations (not Phase 1)

1. **Static collapse**: when all mapping keys in a chain are known
   at compile time, pre-hash to a constant. Most relevant for
   constructors with literal indices.
2. **No-alias-crossing collapse**: when a mapping chain appears
   textually without an intervening alias boundary, compile to
   ONE composite sha256 (today's model). Per-layer becomes the
   fallback only when crossing an alias edge. Optimization, not
   correctness.
3. **uint64 mapping keys skip pad-to-32**: for keys that fit in
   uint64, hash the 8-byte itob directly. Saves 24 bytes per
   hash input.

## What this fixes

1. **Storage pointer aliases through inheritance specifiers**
   (`A(m[1])` pattern, `test_array_mapping_abstract_constructor_param`).
   The alias's value is just a slot-pointer bytes blob; continuing
   the chain in `A`'s body chains naturally. The current
   "alias-prepend-parts" extraction (commit 0e7ffbb30) goes away.

2. **Internal-function storage-pointer params**
   (`test_mapping_array_internal_argument` and family). Caller
   computes the slot pointer; passes as `bytes` arg. Callee chains.
   Same simple shape as inheritance specifier.

3. **Nested storage shapes generally**. Any composition of mapping
   + array + struct + dynamic array becomes one consistent
   derivation rule per layer instead of the current
   "containsMapping"-driven classification.

4. **Reader/writer agreement is structural**. Per-layer derivation
   is the same code path for `m[k]` whether you're writing via
   `state.m[k] = v` or reading via the auto-getter. No more
   matching-key-encoding bugs (the canonicalization fix at
   285ae7d69 / 0e7ffbb30 becomes a non-issue).

## Open design questions

### 1. Slot-numbering scope

Globally unique per contract (one counter shared across all
inherited vars) or per-state-var-declaration (inherited vars keep
their declaring-contract's slot)?

Solidity does the latter via linearized MRO — base class A's
slot 0 stays slot 0 in derived class B even if B declares more vars
first in its own source. **Recommend matching Solidity** — keeps
storage layout predictable when ports are loaded against multiple
inheritance hierarchies, and the upstream Solidity tests will
already use this convention.

### 2. Field packing

Solidity packs multiple small fields into one slot (e.g.
`uint128 + uint128` → one slot). Saves slot count; complicates
codegen for partial reads/writes; matters less on AVM where each
box has a fixed 2-byte minimum overhead anyway.

**Recommend skip initially.** Adds layout-arithmetic complexity
across the codegen for limited payoff. Revisit if a test fixture
relies on packing-specific behavior (gas tests don't apply to AVM
so the only signal would be `sstore(p, ...)` Yul tests that read a
packed-slot raw byte pattern).

### 3. Migration plan

Two paths:

**(a) Big-bang in a feature branch.** Rewrite `StorageMapper` +
`SolIndexAccessHandlers` + `PublicGetterBuilder` + `SolAssignment*`
+ multi-box-storage handling + transient-storage handling in one
go. Expect ~50–100 tests to flicker through different failure
modes during the migration, settle, land as one large commit
series. Multi-day; risky to land partially.

**(b) Per-shape rollout (recommended).** Introduce slot
infrastructure first (no behavior change — slots are allocated but
not yet used in key derivation). Then migrate one shape at a time
with regression passes between each:

1. Slot allocator + plumbing (allocate but don't emit, NOP commit).
2. Plain `mapping(K=>V) m` → migrate.
3. `T[N] arr` → migrate (linear offset arithmetic).
4. `T[] arr` → migrate (dynamic array head/body split).
5. Composite shapes (`mapping(K=>V)[N]`, `T[N][M]`, etc.) — these
   become "just chain the rules" once primitives work.
6. Structs (field offsets within slot or per-field slots).
7. Internal-function + inheritance storage-pointer args switch to
   slot-pointer-bytes ABI.
8. Auto-getter rewrite uses the chain.
9. Storage aliases via slot pointer values.
10. Cleanup: remove the alias-prepend-parts hack from
    `handleMappingAccess`, remove `containsMappingType` widening
    if obsolete.

Each step is its own commit with `currently fails` markers updated
as tests flicker. Reviewable per layer.

### 4. Transient storage

Today: scratch-slot-packed (its own scheme, a packed-blob in
scratch slot TRANSIENT_SLOT). Migrate too?

**Recommend yes** — same slot numbering applies; slots map to
positions within the transient packed blob (or to separate scratch
slots if we abandon the packed-blob layout). Keeps mental model
unified.

### 5. AppGlobal storage

Today: small scalars live in AppGlobal keyed by encoded var name
(not boxes). Migrate?

**Recommend keep current AppGlobal behavior, just key by
`itob(slot)` instead of encoded name.** Same slot numbering, no
derivation change (AppGlobal entries don't have nested layers).
Big-picture consistency without changing the AppGlobal subsystem's
shape.

### 6. Naming

Today's terminology: "box key prefix" / "box key" / "holder name".
New terms could be "slot pointer" / "slot bytes" / "storage
handle". Affects ~30 call sites' readability.

**Recommend introduce "slot pointer" as the formal term** — matches
Solidity's mental model and makes the per-layer transforms
self-documenting in code review. Keep "box key" as a usage term
(the slot pointer IS the box key when stored).

## Touchpoints (estimated)

C++ files needing edits per the per-shape rollout:

- `src/builder/storage/StorageMapper.{h,cpp}` — slot allocator, slot
  registry, slot-pointer helpers.
- `src/builder/storage/SlotAllocator.{h,cpp}` — new file, maybe
  inline in StorageMapper.
- `src/builder/sol-ast/exprs/SolIndexAccessHandlers.cpp` — rewrite
  `handleMappingAccess`, `handleRegularIndex`. Remove
  alias-prepend-parts logic.
- `src/builder/contract/PublicGetterBuilder.cpp` — rewrite
  dispatch walk to chain layer transforms.
- `src/builder/sol-ast/exprs/SolAssignment*.cpp` — write paths use
  chain.
- `src/builder/sol-ast/exprs/SolIdentifier.cpp` — state-var
  identifier emits slot-pointer bytes constant.
- `src/builder/contract/ApprovalProgramBuilder.cpp` — inheritance
  specifier passes slot pointers.
- `src/builder/sol-ast/calls/SolInternalCall.cpp` — internal call
  passes slot pointers.
- `src/builder/contract/FunctionBuilder.cpp` — internal function
  signature, mapping-key-param registration.
- `src/builder/sol-eb/AsaIntrinsics.cpp` — if any storage refs flow
  through stdlib intercepts (probably not).
- `src/builder/storage/StorageBackend.{h,cpp}` — backend dispatch.
- `src/builder/storage/TransientStorage.cpp` — transient migration.
- `src/json/AWSTSerializer.cpp` — possibly new node types if slot
  pointers need a specific AWST representation.
- Multi-box storage in `SolIndexAccessHandlers.cpp` + downstream —
  reconcile with slot-pointer model.

Estimated LOC: ~1000-1500 net change (mostly rewriting existing
code, some additions for the new abstractions).

## What gets simpler

- The "containsMappingType" widening (across 3 files) becomes
  unnecessary — every storage-ref param is just a slot pointer.
- The alias-prepend-parts logic in `handleMappingAccess` (50+ lines
  added in commit 0e7ffbb30) goes away.
- `StorageAlias` enum kinds collapse: every alias is just a
  slot-pointer expression. `MappingHolder`, `StateRead`,
  `IndexedPath`, `FieldPath`, `TupleSlice` could all be one
  `SlotPointer` kind.
- `mappingKeyParam` registry (separate from `storageAliases`)
  unifies into the same alias mechanism.
- `multi-box storage` (`name + itob(page)` for oversized fixed
  arrays) becomes the natural extension of the dynamic-array case.

## What gets harder

- Migration from the existing test corpus produces churn — each
  test that worked under composite-key derivation now needs
  per-layer derivation to produce matching keys. No test fixture
  changes needed (the contracts are EVM-Solidity), but the
  generated TEAL changes.
- Per-layer hashing is more sha256 calls at runtime. Each layer
  adds 35 opcodes for sha256. For deep nests this is meaningful.
  Mitigation: combine sequential mapping layers into one sha256
  when statically known not to cross the alias boundary (an
  optimization, not a correctness requirement).
- Dynamic arrays with one box per element multiplies the box-ref
  count significantly. AVM's per-txn box-ref budget is 8; cross-app
  pooling helps but isn't free. Existing single-box-for-whole-array
  was a deliberate AVM-side optimization. Need to weigh.
