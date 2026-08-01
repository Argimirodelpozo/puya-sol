# EVM-layout compatibility mode (`--evm-storage-layout`)

**Status: STAGE 1 PROTOTYPE LANDED** (2026-08-01; proposed 2026-07-30 as design-only).

Implemented behind `--evm-storage-layout`, exactly the hybrid design below:

- **Dispatch** (`StorageDispatch.cpp::buildEvmSlotStorageDispatch`):
  `__storage_read/write` = slot < 2^16 → 2048-byte page box `"p:" ++ itob(slot/64)`
  (lazy create on write, absent page reads 0 — no MBR on reads); else sparse
  `"s:" ++ slot32`. Plus `__evm_bytes_read/write`: Solidity's short/long
  string storage format (short: data ++ 2·len in the word; long: 2·len+1 at p,
  chunks at keccak256(p); shrink zeroes stale chunks).
- **Lowering** (`sol-ast/EvmSlotLowering.{h,cpp}`): one recursive slot-lvalue
  resolver (state var → mapping keccak256(key32‖slot32) chains → dyn-array data
  at keccak256(slot32) → struct member offsets → packed sub-word windows) +
  read/write emitters through SlotWordCodec. Hooked at: SolIdentifier,
  SolIndexAccess, SolFieldAccess, SolLengthAccess, SolAssignment
  (`tryHandleEvmStorageWrite`, incl. compound), SolUnaryOperation (++/--/
  delete), SolArrayMethod (push/pop, EVM zero-on-pop), ctor initializers.
- **Asm coherence**: named-cell SlotRoutes / stateVarSlots / box sentinels are
  disabled in-mode; `.slot`/`.offset` still resolve to real layout constants,
  so sload/sstore and Solidity codegen address the SAME words. Verified on
  localnet: asm `sstore` visible to high-level reads, asm
  `keccak256(key‖slot)` mapping writes read back via `m[k]`, Checkpoints-style
  `add(keccak256(p),i)` reads, exact EVM packed-word bytes, exact EVM
  short-string word bytes.
- **ARC-56**: per-var state declarations suppressed (only `__ctor_pending`
  remains); auto-getters for public vars skipped with a warning (explicit
  getters required for now). Immutables/constants/transients keep their
  existing named-cell/scratch models (they are not EVM storage).
- **Address caveat**: an `address` ALONE in its slot stores all 32 AVM bytes
  (round-trips real accounts); a PACKED address keeps the EVM trailing-20
  convention (lossy for real AVM addresses — documented divergence).
- **Tests**: `tests/puyasolRegression/test_puyasol_regression.py::test_evm_layout_*`
  (6 green on localnet) + fixture `contracts/evm_storage_layout.sol`; harness
  gained `extra_args` pass-through (cache-keyed).

**STAGE 2 LANDED** (same day): storage-ref params, locals, and returns are
uniformly **biguint slot handles** in-mode:

- Contract methods, libraries and free functions type storage params/returns
  as biguint (FunctionBuilder / AWSTBuilder); call sites pass
  `EvmSlotLowering::resolve(arg).slot` (SolInternalCall); the write-back
  augmentation machinery is bypassed entirely — slot handles write straight
  through. `return <storage expr>` returns the slot; named storage returns
  zero-init to biguint and synthesize as biguint.
- Storage LOCALS bind as `name = slot` biguint vars (SolVariableDeclaration),
  registered as slot-storage refs; pointer REBINDS (`p = poss[k2]`) are
  runtime slot assignments — safe in conditionals, unlike the named-cell
  model's compile-time alias rebinding.
- Asm: every storage local/param's `.slot` resolves to its biguint var
  (`.offset` → 0); `r.slot := e` was already a biguint var assignment. The
  StorageSlot alias interception and the contract-method storage-param guard
  are bypassed in-mode (slots make both unnecessary).
- Whole-STRUCT materialisation (storage → memory copies, incl.
  `Checkpoint memory last = _unsafeAccess(self, pos-1)`), struct-element
  push/pop (EVM zero-on-pop), type-conversion peeling (`bytes(a).length`).

**Real-contract score** (the blockers in §1): **kaito ✓, usde ✓ (OZ
StorageSlot/ShortStrings path), degen ✓ (OZ Checkpoints/ERC20Votes) — all
compile end-to-end to TEAL under the mode.** builder remains blocked on the
MEMORY half (stage 3: asm arithmetic on memory string/array values) plus
`codesize()`/`extcodesize()`, which are unfixable AVM gaps regardless of mode.

Verified on localnet: `test_evm_layout_storage_ref_params` — StorageSlot
write-through (direct + through a `string storage` library param),
`getUint256Slot(bytes32(4))` aliasing a declared var, Checkpoints
using-for push/latest via asm keccak+add, library array params, storage
locals with runtime rebind.

**Not yet (stage 3)**: universal blob memory (asm pointer arithmetic on
memory strings/arrays — what builder needs), bytes/string element access &
push/pop, whole-ARRAY materialisation, public auto-getters, `layout at`
bases ≥ 2^16, chainwide-differ integration (its `read_avm_maps` storage
comparison still assumes ARC-56 box-map declarations; in-mode it should
switch to direct slot-map comparison — §5).

Force puya-sol's *internal* storage (and optionally memory) layout to emulate
Solidity's, backed by AVM boxes, so that inline assembly which does real slot
and pointer arithmetic compiles and runs faithfully.

This is a **mode**, not a new default — see "What it breaks".

---

## 1. Why

Every remaining *fixable* real-world blocker found by the chainwide replay sweep
is the same shape: assembly manipulating a storage slot or memory pointer as a
NUMBER, which puya-sol's logical model has no value for. The compiler correctly
refuses rather than miscompiling ("cannot coerce non-scalar type 'X' to biguint
in assembly arithmetic", "unmodeled .slot reference"), but the refusal is what
gates the contracts.

Observed blockers, all dissolved by this mode:

| Idiom | Contracts blocked | Today |
|---|---|---|
| OZ `StorageSlot.getStringSlot(store).value = v` (`r.slot := store.slot`) | kaito, usde | hard error (assignment target lowered to a constant) |
| OZ `Checkpoints._unsafeAccess` (`add(keccak256(0,0x20), pos)`) | degen, builder | hard error (`dynamic_array<T>` → biguint) |
| `dynamic_array<T>` as an assembly value | degen, builder | hard error |
| packed sub-word `sload` | (residual divergence) | documented gap |

These are not exotic. `StorageSlot` is on the OZ EIP-712 / ShortStrings path,
which means most modern ERC-20s with `permit`. `Checkpoints` is every
ERC20Votes token.

If a slot is a real 32-byte number, all of the above is ordinary arithmetic and
the whole `ensureBiguint`-refusal class disappears at once.

---

## 2. Storage model

EVM storage is `uint256 slot -> bytes32 value`. A 32-byte slot number fits the
AVM box-name limit (1–64 bytes) **exactly**, so the naive mapping is direct:

```
sload(k)     = box_get(itob256(k))  // absent => 0, matching EVM
sstore(k, v) = box_put(itob256(k), v)
```

### 2.1 The binding constraint is box REFERENCES, not MBR

Verified AVM rules (see the `box-ref-io-budget` notes): **each box reference
grants 2048 bytes** of read+write budget; duplicate `(app, name)` refs dedupe
within a transaction; refs pool across a group (8 per txn, so 128 across a
16-txn group).

One box per slot means **one reference per distinct slot touched**:

- ERC-20 `transfer` touches 2–3 slots → fine.
- A loop over a 50-element array touches 50 slots → 50 refs → **impossible in a
  single transaction**, and awkward even with a padded group.

MBR is the secondary cost: `2500 + 400 x (len(name) + len(value))`
= `2500 + 400 x 64` = **28,100 µAlgo ≈ 0.028 ALGO per live slot**.

### 2.2 Paging fixes refs, but must not be uniform

Page a run of slots into one box: name = `slot >> 6`, value = 2048 bytes
= 64 slots = exactly one reference's budget.

| Layout | MBR | box refs |
|---|---|---|
| 1 box / slot | 0.028 ALGO per slot | 1 per slot ❌ |
| paged, **dense** page | 0.013 ALGO per slot | 1 per **64** slots ✓ |
| paged, **mapping entry** | **0.83 ALGO per entry** ❌ | 1 |

A page box must be allocated at full size even when one slot in it is live.
Mappings and dynamic arrays hash to *random* slots, so under uniform paging each
mapping entry would own its own sparse 2048-byte page — 0.83 ALGO per entry, a
severe regression against today's per-entry box.

### 2.3 Proposed: hybrid, split at RUNTIME on the slot value

Solidity assigns declared state variables sequential slots from 0. Hashed
regions (mapping entries, dynamic-array bodies) live at keccak outputs, which
are astronomically large. So a single comparison on the slot number separates
them with no static analysis at all:

```
sload(k):
    k < DENSE_LIMIT  ->  box_extract(page(k >> 6), (k & 63) * 32, 32)   // paged
    otherwise        ->  box_get(itob256(k))                            // sparse
```

with `DENSE_LIMIT = 2^16` (generous: no real contract declares 65,536 top-level
slots, and no keccak output is anywhere near it).

- dense/sequential state, structs, fixed arrays → paged, 1 ref covers 64 slots
- mapping entries, dynamic-array bodies → one box per 32-byte slot key, roughly
  today's per-entry cost

This matches how EVM contracts actually lay out, so the common case is the cheap
case in both dimensions.

### 2.4 Open questions (must be answered before implementing)

- **Page allocation**: create a page box lazily on first write to any slot in
  it. Does a read of an absent page cost anything beyond a `box_get` miss?
- **Sub-word packing**: Solidity packs several small vars into one slot. In this
  mode that becomes *free* (it is just byte offsets within the 32-byte value),
  which is the point — but the existing sub-word helpers assume the logical
  model and would need a parallel path.
- **`DENSE_LIMIT` and page size** should be tunable; 64 slots/page is chosen to
  exactly match the 2048 B reference budget, so it should probably be fixed.
- **Opcode budget**: every `SLOAD`/`SSTORE` becomes a box op. Contracts already
  near the 700-op budget will need more opup; measure before committing.
- **Group-level ref planning**: who computes the box references a call needs?
  Today the harness/caller declares them. A flat slot space makes the set
  data-dependent, which may need a conservative over-declaration pass.

---

## 3. Memory model

**Largely already built** — this half is much smaller than it sounds.

`blob-memory-model` gives flat, byte-addressed EVM memory in AVM scratch slots
(`mload` = `extract3`, `mstore` = `replace3`, dynamic offsets native), and
`multi-slot-memory-model` lifts that past 4 KB with a blob-aggregate pointer
model for locals, params and returns.

What exists today is a **selective** gate: `markAssemblyAggregates`
(`src/builder/contract/ContractBuilder.cpp`) blob-backs an aggregate only when
it is a real array, or a `new`-allocated bytes/string buffer whose pointer
escapes into a Yul local. Everything else keeps the value model, because the
value model is faster and preserves `x[i] =` / `.length` / `return x`.

So the memory half of this mode is mostly: **flip that gate from selective to
universal**. The cost is not correctness but performance and blast radius —
every `tryHandleBytes*` value-model handler stops being reachable, so the
pointer model must cover all of their cases (that is what makes it a larger
change than it first appears, and why it is worth staging separately).

---

## 4. What it breaks

- **ARC-56 state introspection by name disappears.** State becomes opaque
  numbered slots, so `state.keys.global` / `state.maps.box` can no longer be
  declared meaningfully. That conflicts with ARC-20 conformance and with the
  box-map declarations `chainwide-historical-diff/avm_leg.read_avm_maps`
  depends on. **This is the reason it must be a mode, not a default.**
- **Not mixable within one contract.** The existing model derives box keys by
  hashing the variable name; the two schemes cannot coexist for one state var.
- **Opcode budget** per SLOAD/SSTORE, as above.
- **MBR** grows for storage-heavy contracts even in the hybrid layout.

---

## 5. Verification — already in place, and it gets *stronger*

The decisive advantage: in this mode both legs of
`tests/chainwide-historical-diff` hold **literally the same slot → value map**.
Storage diffing stops being name-based alignment (mapping the AVM's
box-key-by-variable-name model onto solc's `storageLayout`) and becomes an exact
slot-for-slot comparison — total coverage by construction, with no
"uncompared map" or "blind slot" caveats at all.

The existing SSTORE trace already records the exact set of slots each txn wrote
on the EVM leg. In this mode that trace is directly comparable to the AVM leg's
box writes, which is about as strong an oracle as this project can have.

Sequence to validate a prototype:
1. `selftest.py` — synthetic contract, all four mapping value shapes.
2. `fuzz_evm.py` on the assembly-heavy fixtures (ShortStrings, Checkpoints).
3. Chainwide replay of the contracts this mode is supposed to unlock
   (kaito, usde, degen, builder) plus the 16 already-green ones as a
   no-regression gate.

---

## 6. Suggested staging

1. **Storage only, behind `--evm-storage-layout`**, hybrid paged/sparse. No
   ARC-56 state declaration in that mode. Biggest unlock per unit of work.
2. Measure opcode/MBR cost on the already-green contracts before widening.
3. Memory universal-blob mode only if (1) proves out and a real contract still
   needs it.

Related notes: `box-ref-io-budget`, `blob-memory-model`,
`multi-slot-memory-model`, `asm-mstore-length-word`,
`asm-slot-storage-ref-param`, `ensurebiguint-strict-assembly`,
`storage-slot-model-s1`, `mapping-box-key-collision`, `arc20-conformance`.
