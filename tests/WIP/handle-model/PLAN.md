# Aggregate Handle Model — re-architecture plan

**Goal:** model storage & memory aggregates (struct/array) as **references** (small handle
tuples) instead of values, so Solidity's data-location semantics hold: storage refs passed to
functions write through; memory→memory assignment aliases; deep copies happen *only* at
Solidity's copy-points; references are kept unless a write forces materialization.

This replaces the current model where aggregates are immutable ARC4 byte-blobs (`WType
m_immutable=true` → value-copy on assignment) + the copy+write-back augmentation for storage
refs (library/free only) + the >4KB blob/pointer model for memory.

## Representation (the handle is a tuple)

- **Storage handle = `(boxKey, offset)`** — leaf read `box_extract(key, offset, len)`, leaf
  write `box_replace(key, offset, encodedLeaf)`. (Both AVM opcodes exist; `makeBoxExtract` +
  `makeBoxReplace` are the AWST helpers.) Field/index access derives a new offset.
- **Memory handle = `(scratchRegion, offset)`** — leaf read/write via the mem-word ops
  (`readMemWord`/`writeMemWordDirect`). This is exactly today's >4KB blob/pointer model,
  generalized to every size.
- Both are `(backing, offset)` — a unified `Reference` shape; the leaf op dispatches on the
  backing kind. `offset` is a compile-time constant for fixed-size ARC4 layout; **runtime**
  (read head-pointer → tail) for dynamic-size (dynamic arrays, string/bytes fields).

## Copy semantics (Stage 3)

Insert a deep copy at exactly Solidity's copy-points; alias everywhere else:
- alias (copy the handle): memory→memory, `S storage s = …` local, storage/memory ref param.
- deep copy: storage→memory, memory→storage, value assignment of a storage aggregate.
- **elision:** if a mutation analysis (extend `ParamMutationDetector`) proves a ref is
  read-only, skip even the required copy; else keep the handle and copy-on-write at first write.

## Staging (each shippable; gate = differential battery + full zero-reg)

- **Stage 0 — primitives (DONE).** `makeBoxReplace` added (`src/awst/Node.h`, mirrors
  `makeBoxExtract`). Confirmed `box_extract`/`box_replace` opcodes + `resolveBlobOffset`
  (`SolIndexAccess.cpp:204-258`) already implement the `path→offset` math (root + i*stride +
  Σ field-sizes). Pure addition — no behavior change.
- **Stage 1a — box-backed storage refs.** Aggregates already in boxes (dynamic arrays, large
  structs, mapping values) flow as `(boxKey, offset)` handles through locals/params/returns;
  leaves via box_extract/replace; retire copy+write-back for these. NO storage-layout change →
  lowest risk. Fixes battery `storageParamMutates` / `arrayParam` / `arrayElemParam`.
- **Stage 1b — app-global small structs.** The `structVarParam` case. Decision: discriminated
  handle (`box | appglobal`, app-global leaf = whole-value get + arc4-replace-at-offset + put)
  **vs** box-everything (uniform handle, layout change + box-ref-budget cost). Lean
  discriminated (no layout change → existing storage behavior untouched → zero-reg stays green).
- **Stage 2 — memory refs.** All memory aggregates → `(region, offset)` handles; memory→memory
  aliasing correct. Generalize the blob/pointer model down to every size. Fixes `memArrAlias` /
  `memStructAlias`.
- **Stage 3 — copy semantics + elision.** Per the rules above. Battery is the oracle.

## Seams (where it plugs in — from the data-model map)

1. `isBoxKeyedStorageRef` (`StorageRefPointer.h:88-94`) — the chokepoint forking bytes-box-key
   vs by-value; consulted at 5 sites (`AWSTBuilder.cpp:267`, `SolInternalCall.cpp:159,353`,
   `FunctionBuilder.cpp:446`, `SolExpressionStatement.cpp:167`). A handle subsumes this.
2. Registries `mappingKeyParams` (`Context.h:100`) + `blobAggregates` (`Context.h:102-105`) —
   the existing partial "storage handle" + "memory pointer" stores; unify into the handle.
3. `SolInternalCall::buildSubroutineCall` (`:320-595`) — the hand-assembled, order-coupled
   calling convention (root tracing + tuple packing + per-root write-back); the handle becomes
   the ABI, retiring all of it.
4. `resolveBlobOffset` (`SolIndexAccess.cpp:204-258`) — the offset resolver; generalize to a
   shared `path→(backing,offset)` for storage + memory.
5. `SolVariableDeclaration.cpp:96-224` — binds a returned handle into a local; 4 shapes
   (StorageAlias kinds / mapping-key bytes / slot biguint / blob uint64) collapse into one.

## Validation

Differential battery (`tests/WIP/tiny-fuzzing-oracle/`): `fuzz_evm.py contracts/repro_locations.sol`
(+ loc_aliasing, nested_agg, storage_packing) diffs AVM vs live solc+EVM. Plus full pytest
zero-reg (baseline 58f / 1245p / 87xf). The battery from the (A) investigation is the oracle.

## Status

- Stage 0: DONE (primitive added; building to confirm compile).
- Next: Stage 1a — the storage-rooted offset resolver + wiring box-backed refs to flow as
  handles, tested against the array cases in the battery.
