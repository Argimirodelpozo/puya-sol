# puya bugs found while porting AAVE V4

## 1. AVM `b>>` doesn't match EVM `shr` for shifts ≥ bit-width

**Status:** Open. Workaround: 2 tests xfail-strict in
`WIP/examples/aave-v4/test/test_position_status_map.py` under
`_BOUNDARY_SHR_XFAIL`.

### What

Yul `shr(N, X)` on EVM returns `0` whenever `N ≥ 256` (logical right
shift; bits past the end fall off). On AVM `b>>` (the biguint shift
right that puya emits for Yul's `shr`) does **not** clamp to zero
when the shift amount is ≥ the operand's bit width. The result is
non-zero, leaving the operand effectively unmasked.

### Where it bites in AAVE V4

`PositionStatusMap.isolateCollateralUntil` /
`isolateBorrowingUntil` (`WIP/examples/aave-v4/contracts/PositionStatusMap.sol:218,229,244`):

```yul
ret := and(word, shr(sub(256, shl(1, mod(reserveCount, 128))), MASK))
```

For `reserveCount % 128 == 0` (i.e. exactly on a bucket boundary —
128, 256, 384, ...) this simplifies to `shr(256, MASK)`. EVM: 0 →
AND with anything = 0 → no bits leak from the boundary bucket.
AVM: non-zero → mask leaks → boundary bucket's bits are counted
when they shouldn't be.

### Reproduction

```python
# test_position_status_map.py::test_collateralCount
p.setUsingAsCollateral(127, True)        # bit at reserveId 127
p.setUsingAsCollateral(128, True)        # bit at reserveId 128
assert p.collateralCount(128) == 1       # FAILS: gets 2
# Expected 1 because reserveCount=128 means "indexes 0..127 valid,
# 128 not yet listed". The `shr(256, MASK)` was supposed to zero
# the mask for the boundary bucket; AVM doesn't.
```

### Where to look

puya-sol's lowering of Yul `shr`. Likely in:
- `src/builder/sol-ast/assembly/BitwiseShiftOps.cpp`
- or wherever `handleShr` lives in the assembly translator

When the shift amount is a constant ≥ 256 — or a runtime value that
*could* be ≥ 256 — emit `if shift >= 256 then 0 else b>>(shift, value)`
instead of a bare `b>>`. EVM-faithful would clamp at 256 unconditionally,
even for runtime shifts.

The `b<<` direction may have the symmetric issue (Yul `shl(N, X)` for
`N ≥ 256` is also 0 in EVM); worth checking while you're there.

### Affected tests (will pass automatically once fixed — xfail strict)

- `WIP/examples/aave-v4/test/test_position_status_map.py::test_collateralCount`
- `WIP/examples/aave-v4/test/test_position_status_map.py::test_borrowCount`

### Related (unrelated puya bugs found, already worked-around)

- `m[--var]` codegen: prefix-decrement-in-mapping-subscript caused
  TWO `b-` ops because puya-sol's biguint key build referenced
  `cat = concat(pad, translated)` twice (once each in `len(cat)`
  and `extract3`). Fixed in `src/builder/sol-ast/exprs/SolIndexAccessHandlers.cpp`
  by materialising side-effecting AssignmentExpression indices to a
  fresh local var via prePendingStatements. (Tried `SingleEvaluation`
  first — puya's SE cache didn't deduplicate after JSON round-trip
  even with matching ids and structurally-equal sources.)

---

## 2. Default `NewStruct` with `arc4.bool` fields emits `getbit("", 0)`

**Status:** Open in puya. Worked-around in puya-sol by emitting a
zero-filled `BytesConstant` directly for fixed-size ARC4Struct
defaults, sidestepping puya's broken encoder.

### What

Puya's encoder for a literal `NewStruct{...}` whose fields include
`arc4.bool` packs consecutive bools into bits via `setbit` on a
running bytes buffer — but starts the buffer as the **empty
BytesConstant** `bytec_1 // 0x` instead of `bzero(1)` (or whatever
the correct byte width is for the bool group). The first bool's
encoding therefore emits

```
bytec_1 // 0x        ; running buffer = ""
intc_2 // 0          ; bit index 0
getbit               ; getbit("", 0) — ALWAYS errors at runtime
bytec_1 // 0x        ; ""
intc_1 // 1          ; bit index 1
uncover 2; setbit    ; setbit("", 1, prev) — also errors
```

`getbit` and `setbit` reject byte-string indices past the slice's
length, so any execution path that constructs the struct default
panics with `getbit index beyond byteslice`.

### Where it bites in AAVE V4

Hub's box-backed mappings of `SpokeData` and `Asset` (which both
carry consecutive `bool active; bool halted;` fields). Any view
method that reads from an uninitialized mapping entry — e.g.
`Hub.getSpokeAddedAssets` / `Hub.getSpokeAddedShares` from
`TreasurySpoke.getSuppliedAmount` etc. — triggers the StateGet
default path, instantiates a default `SpokeData`, and crashes
during the encoder.

The chunked TEAL for `getSpokeAddedShares` shows it clearly
(`WIP/examples/aave-v4/out/Hub/__uros_split/chunk_1/Hub__chunk_1.approval.teal:1987-2001`):

```
bytec_1 // 0x
intc_2 // 0
getbit                ; <- crashes here on any unset _spokes box
bytec_1 // 0x
intc_1 // 1
uncover 2
setbit
bytec 5  // 73 zero bytes
swap; concat
bytec 6  // 25 zero bytes
concat
swap
box_get
select
```

### Workaround in puya-sol

`src/builder/sol-types/TypeCoercion.cpp` — `makeDefaultValue` for
`ARC4Struct` now mirrors the `ARC4StaticArray` path: when the
struct is fixed-size (no dynamic-length fields), emit a
`BytesConstant` of zeros at the encoded width (with the struct
type as wtype) instead of a `NewStruct`. Also extends
`computeEncodedElementSize` to account for ARC4 bool packing
(consecutive `arc4.bool` fields share a byte; `ceil(N/8)`).

This keeps the encoded bytes identical to what puya *should* have
produced, but skips puya's struct-encoder entirely for the all-zero
case. Dynamic-size structs still go through `NewStruct` and remain
exposed to the bug — none of AAVE V4's mapping value types are
dynamic, so this covers the hot path.

### Suggested upstream fix

In puya's ARC4 struct encoder, initialize the running bool-pack
buffer as `bzero(ceil(group_size / 8))` instead of an empty bytes
constant, so the first `getbit`/`setbit` operates on a valid byte
index.

---

## 3. False-positive "infinite loop detected" for a loop followed by an unconditional revert

**Status:** Open in puya. No clean puya-sol-side workaround — the
AWST and initial SSA IR puya-sol emits are correct; the defect is
puya's optimizer + validator interaction. puya 5.8.0rc3
(submodule `0d5af6b41`).

### What

When a loop is **immediately followed by an unconditional failure**
(`revert` / `err`), puya's optimizer correctly folds the loop's exit
edge into an in-loop `assert`, producing a basic block whose
terminator targets only itself. `NoInfiniteLoopsValidator`
(`puya/src/puya/ir/validation/infinite_loop.py`) then rejects that
block — but it is **not** infinite: the block contains an `assert`
that halts execution once the loop condition goes false. A valid,
terminating program is refused with:

```
<source>:NN error: infinite loop detected
error: puya exited with code: 1
```

### Reproduction

The trigger is specifically *loop → unconditional fail*. With
`return` (or nothing) trailing, it compiles fine:

```solidity
contract T {
    uint8[] x;
    function withReturn() public {          // COMPILES OK
        for (uint i = 0; i < 100; ++i) x.push(uint8(i));
        return;
    }
    function loopOnly() public {            // COMPILES OK
        for (uint i = 0; i < 100; ++i) x.push(uint8(i));
    }
    function withRevert() public {          // FAILS: "infinite loop detected"
        for (uint i = 0; i < 100; ++i) x.push(uint8(i));
        revert();
    }
}
```

Equivalent puyapy shape — any loop whose only exit is an
unconditional `op.err()` / failing assert with nothing after it.

Fixtures hitting it in the puya-sol suite:
`errors/small_error_optimization.sol`,
`array/array_function_pointers.sol`.

### IR trace

The AWST puya receives is correct; the **initial SSA IR is correct
and terminating** (`*.000.ssa.ir`):

```
block@4: // while_top
    let i#1: biguint = φ(i#0 <- block@3, i#2 <- block@5)
    let tmp%5#0: bool = (b< i#1 100b)
    goto tmp%5#0 ? block@5 : block@6      // 2 distinct targets — fine
block@5: ... loop body ...; i#2 = (b+ i#1 1b); goto block@4
block@6: fail // revert
```

Then puya's own optimizer folds the conditional branch whose false
edge is a pure-`Fail` block into an `assert`
(`ir/optimize/control_op_simplification.py` /
`collapse_blocks.py`). By `*.002.ssa.opt.ir`:

```
block@14: // while_top
    let i#1: biguint = φ(i#0 <- block@13, i#2 <- block@14)
    let tmp%5#1: bool = (b< i#1 100b)
    (assert tmp%5#1)                      // exit edge folded into assert
    ... loop body ...
    let i#2: biguint = (b+ i#1 1b)
    goto block@14                         // terminator now targets ONLY itself
```

The fold is **valid** (`if (!cond) fail` ≡ `assert(cond)`); the
merged block iterates while `cond` holds and terminates via the
failing `assert` when `cond` is false.

### Why the validator is wrong

```python
class NoInfiniteLoopsValidator(DestructuredIRValidator):
    def visit_block(self, block: models.BasicBlock) -> None:
        assert block.terminator is not None, ...
        if block.terminator.unique_targets == [block]:
            logger.error("infinite loop detected", ...)
```

It inspects only `block.terminator`. The invariant
"self-targeting terminator ⇒ infinite loop" is sound only for
blocks with no intervening halting op. After the fold, `block@14`
contains `(assert tmp%5#1)` — a mid-block exit.

### Suggested upstream fix

In `NoInfiniteLoopsValidator.visit_block`, skip the report when the
block contains a halting op (`assert` intrinsic, `err`, etc.):

```python
def visit_block(self, block):
    assert block.terminator is not None, ...
    if block.terminator.unique_targets != [block]:
        return
    if any(_can_halt(op) for op in block.ops):   # assert / err / ...
        return
    logger.error("infinite loop detected", ...)
```

Alternatively, prevent the upstream fold from merging an exit edge
into a loop header when that makes the block self-targeting — but
the validator guard is the smaller fix and keeps the optimisation.

## 4. puya 5.9.0rc1 surfaces 4 latent puya-sol AWST bugs

**Status (after follow-up triage):** Not puya-side bugs. The 4 cases
below are **puya-sol AWST shapes that puya 5.8 was tolerant of and
puya 5.9 surfaces as runtime errors** under its stricter optimizer.
3 of 4 fixed in puya-sol (4a + 4b: AbiEncoder/Pow paths;
4c: StorageMapper/PublicGetterBuilder skip the StateGet-default
branch for **statically oversized** box-backed types so puya stops
emitting `bzero(N)` with N > 4096).
4d (`uint[]` growing past 4 KB at runtime) remains open — same
`StateGet` issue but for dynamic types that we can't statically
classify as "always oversized". Tried extending the skip to all
dynamic types in v294; that broke 3 tests that rely on the
StateGet empty-default for first-read-before-write on dynamic
bytes/arrays (puya-sol intentionally skips eager `box_create` for
those — see `ApprovalProgramBuilder.cpp:790`). Needs a different
approach (explicit `box_exists` check around the slice read, or
eager `box_create` for those types + bare BoxValueExpression).

Original framing ("puya optimizer regressions") was wrong — puya was
faithfully executing the AWST puya-sol asked for; 5.9's new optimization
passes just stopped masking puya-sol's mistakes.

### What

Bumping puya 5.8.0rc3 → 5.9.0rc1 + the corresponding AWSTSerializer
compat changes (ARC4Struct.fields as JSON array of WTypeField,
BoxPrefixedKeyExpression → IntrinsicCall(concat) inline) introduced 4
runtime regressions in the semantic test suite:

#### 4a. `test_create_random` — `extract 7 1` on 1-byte buffer — **FIXED**

Solidity source (`tests/various/contracts/create_random.sol`):
```solidity
function calculateCreate2(address creator, bytes32 codehash, bytes32 salt)
    private pure returns (address)
{
    return address(uint160(uint256(keccak256(
        abi.encodePacked(bytes1(0xff), creator, salt, codehash)))));
}
```

Runtime error:
```
logic eval error: extraction start 7 is beyond length: 1.
Details: extract3; pushbytes 0xff // 0xff; extract 7 1
```

puya 5.9 emits `pushbytes 0xff; extract 7 1` — extract 1 byte at offset
7 from a 1-byte literal `0xff`. Overflows. Worked on 5.8.

**Root cause:** `AbiEncoderBuilder::packArgPacked` (line ~125) emits
`extract(bytesExpr, 8 - packedWidth, packedWidth)` unconditionally for
`packedWidth ∈ [1, 7]` — assuming `bytesExpr` is 8 bytes (from `itob`
of a uint64). For uint64/bool inputs that holds. For `bytes1(0xff)`
inputs the FixedBytesType lowering ALREADY produced a 1-byte value, so
the second extract `extract 7 1` overflows the 1-byte buffer. puya 5.8
tolerated the no-op redundancy; 5.9's optimizer folds the inner extract
into a constant + keeps the buggy outer extract.

**Fix:** capture `inputAlreadyByteshaped` BEFORE the wtype-dispatched
moves in `packArgPacked`; gate the truncation `extract` on
`!inputAlreadyByteshaped`. Bytes-typed inputs skip the redundant slice.
Test recovered (v292 → v293).

#### 4b. `test_exp_cleanup_smaller_base` — `exp` overflow on `uint8 ** uint16` — **FIXED**

Solidity source (`tests/cleanup/contracts/exp_cleanup_smaller_base.sol`):
```solidity
function f() public pure returns (uint16 x) {
    uint16 e = 0x100;
    uint8 b = 0x2;
    unchecked { return b**e; }   // 2**256, wraps to 0 in uint16
}
```

puya 5.9 lowers this to AVM's `exp` opcode (uint64-only), which
overflows on `2**256`. puya 5.8 used a wider biguint path. Expected
return: 0; actual: revert.

**Root cause:** `SolIntegerBuilder` uint64 Pow case emitted AVM `exp`
(uint64-only, asserts on overflow) unconditionally, then for unchecked
sub-uint64 widths applied a post-mod 2^m_bits — but the intermediate
`exp` already overflowed before the mod could fire.

**Fix:** for `m_scope.isUnchecked() && !m_signed && m_bits < 64`,
route through `buildBigUIntExp` (biguint square-and-multiply, no
overflow), then mod `2^m_bits` and cast back to uint64. Other
combinations (checked OR full uint64 OR signed) keep AVM `exp`.
Test recovered (v292 → v293).

#### 4c. `test_fixed_arrays_in_storage` — abi_return = None — **FIXED**

#### 4d. `test_array_storage_index_boundary_test` — out-of-bounds
revert expected but didn't fire — **OPEN**

Tested case: `test_boundary_check(uint256, uint256)` grows
`uint[] storageArray` to 256 elements (8194 B encoded) then reads
`storageArray[255]`. The read goes through `StateGet(BoxValue, default)`
which lowers to `box_get` (whole-box load — bounded at AVM 4096-byte
stack value cap, so an 8194 B box reverts).

Cannot reuse 4c's fix because for dynamic types like `uint[]` we can't
statically prove the box exists at first read — puya-sol intentionally
skips eager box_create for dynamic bytes (see
`ApprovalProgramBuilder.cpp:790`). Switching dynamic-type reads to
bare `BoxValueExpression` asserts on missing-box and regresses tests
that rely on the empty-default-on-first-read semantics
(`byte_array_transitional_2`, `mappings_array2d_pop_delete`,
`internal_types_in_library`).

Next steps for 4d:
- (a) Lift the "skip box_create for dynamic bytes" optimisation —
  always eagerly create the empty box in __postInit, then bare
  `BoxValueExpression` is safe. Cost: one extra `box_create 0` per
  dynamic-bytes/dynamic-array state var.
- (b) Emit an explicit `box_exists(key)` check around dynamic-array
  reads; on miss, materialise an empty-bytes / `0x0000` default
  (small — safe on stack); on hit, route through `box_extract` for
  slice reads.
- (c) Stay on puya 5.8.0rc3 for this specific test (already an XFAIL
  candidate).

---

(legacy 4c+4d combined notes from initial framing — superseded by
the per-test notes above)


**Root cause (both):** puya-sol's `StorageMapper::createStateRead`
wraps every box read in a `StateGet` with a typed zero-default —
intended so that reading an uninitialized box returns the Solidity
default (0/false/empty). Under puya 5.9, `StateGet`'s default branch
materialises the full encoded zero of the storage type via
`bzero(N)`. For `Data[1024]` (1024 × 64 = 65536 B), `bzero(65536)`
exceeds AVM's 4096 B stack-value cap and the contract reverts at
runtime — before the actual `extract3` ever runs. puya 5.8 lowered
`box_extract` more directly and never tried to materialise the full
zero default.

For 4d (`test_boundary_check`), the same revert fires inside the
in-bounds check before puya can reach the synthetic OOB check, so the
test reports "no revert at the boundary" — which is really "wrong
revert at the bzero".

**Fix:** in `StorageMapper::createStateRead` (and the equivalent path
in `PublicGetterBuilder` for synthetic public state-var getters),
when the box-backed type's `computeEncodedElementSize > 4096`, skip
the `StateGet+default` wrapper and return `BoxValueExpression`
directly. puya lowers a bare `BoxValueExpression` via `BoxRead` (no
big zero materialisation; asserts the box exists on miss). Since
puya-sol eagerly `box_create`s these state vars in `__postInit` (see
`m_boxArrayVarNames` in `ApprovalProgramBuilder`), the assert never
fires in practice. Threshold lives as
`StorageMapper::kAvmStackValueMax = 4096`.

Trade-off: oversized box types lose default-on-missing-box semantics,
but those types couldn't be read at all under puya 5.9 before the
fix, so this is strictly more permissive.

### Where to look (puya-side)

These look like the new optimization passes in puya 5.9. Likely
suspect commits between puya 5.8.0rc3 (`0d5af6b41`) and 5.9.0rc1
(`2ed95fdff`):
- `e93308c3c fix: consistently rely on optimisation to replace stack-arg
  variant with immediate-arg variant`
- `1cf62c121 refactor: add special handling for extract in eb`
- `69f1214d5 test: add tests for op code selection optimisation
  preferring variant with immediates`

The `extract 7 1` overflow specifically smells like the new immediate-
variant selection going wrong on a literal too short to slice.

### Affected tests

- `tests/various/test_various.py::test_create_random`
- `tests/cleanup/test_cleanup.py::test_exp_cleanup_smaller_base`
- `tests/array/test_array.py::test_fixed_arrays_in_storage`
- `tests/array/test_array.py::test_array_storage_index_boundary_test`

### Workaround (partial)

Stay on puya 5.8.0rc3 (`0d5af6b41`) for the 4d-specific case
(dynamic array growing past 4 KB). 4a + 4b + 4c are fixed on the
puya-sol side; the puya 5.9 bump now passes them cleanly. Only 4d
remains regressed under 5.9 vs the 5.8 baseline.

## 5. address-typed parameter → uint64 over-elision in inner-call ApplicationID

**Status:** Open in puya 5.8.0rc3 AND 5.9.0rc1 (same misbehavior under
both). Workaround in PE: their SimpleSplitter (added in
PE commit `f7870b9b1`) auto-extracts subroutines to helpers,
side-stepping the orch code path that triggers this. No clean
workaround in main puya-sol yet.

### What

When a Solidity inner-call uses an `address`-typed parameter as the
receiver — like `IPolyProxyFactory(_proxyFactory).getImplementation()`
— puya-sol emits the correct AWST chain to extract the app id from the
address bytes:

```
ReinterpretCast(application,
    btoi(
        extract(bytes, [24, 8],
            ReinterpretCast(bytes, _proxyFactory:account))))
```

That's: account → ReinterpretCast(bytes) → extract(24,8) → btoi
→ ReinterpretCast(application). At runtime, the 32-byte account value
gets sliced (bytes[24..31]) and converted to uint64 app-id.

puya's optimizer folds `extract 24 8; btoi` into either
`pushint 24; extract_uint64` (stack form, takes bytes from stack +
uint64 offset) or `extract_uint64 24` (immediate form, takes bytes from
stack). Both correct, both faster.

**The bug:** further upstream in the same optimization pass, puya
elides the outer `ReinterpretCast(bytes, ...)` because account is
bytes-backed already — but ALSO elides somewhere that causes the value
on stack to land as **uint64** instead of bytes by the time the
`extract_uint64` runs. Runtime error:

```
logic eval error: extract_uint64 arg 0 wanted []byte but got uint64
Details: app=…, pc=…, opcodes=itxn_begin; pushint 24; extract_uint64
   (or: dup; pushint 24; extract_uint64)
   (or: dig 2; pushint 24; extract_uint64)
```

Variant stack-manipulator opcode (`dup` vs `dig 2` vs nothing) depends
on surrounding state-var-write context, but the underlying type
mismatch (uint64 on stack where bytes expected) is constant.

### Where it bites

Polymarket v2 `__postInit` runs `PolyFactoryHelper`'s constructor:

```solidity
proxyImplementation = IPolyProxyFactory(_proxyFactory).getImplementation();
address _safeImpl = IPolySafeFactory(_safeFactory).masterCopy();
```

Both inner-calls trigger the bug. The bug is independent of
`--uros-splitter` use; standalone v2 CTFExchange compile reproduces
it.

### Where to look (puya-side)

Likely in puya's IR optimization pass that folds `extract 24 8; btoi`
into `extract_uint64`. The pass needs to ensure the value on stack
just before the `extract_uint64` is still bytes-typed; right now it
allows an earlier elision of the bytes-cast that makes the value
uint64.

### PE workaround vs main reality

PE puya-sol's SimpleSplitter (commit `f7870b9b1`) auto-extracts pure
subroutines into helper contracts. The orch's remaining TEAL ends up
with different stack management around the inner-call (PE generates
correct `swap; pushint 24; extract_uint64` because the value layout
in the smaller orch is different). The "fix" in PE is therefore a
splitter-architecture side-effect, not a targeted puya-or-puya-sol
patch. Porting SimpleSplitter to main is multi-day work
(1290 LOC + supporting modules).

### Source-side workaround (tried, didn't help)

Reordered `PolyFactoryHelper` constructor to do inner-calls BEFORE
state-var writes — generated `dup; pushint 24; extract_uint64` (stack-
top variant) but the value on top is still uint64. The bug is in the
type-tracking of the address parameter through puya's IR, not in the
stack manipulation around the inner-call. Reverted the reorder.

### Reproduction

Compile any contract that calls an inner method via an
`address`-typed parameter from a constructor or method, e.g.
`IFoo(addr).bar()` where `addr` is a function param. Deploy and
invoke. Fails with extract_uint64 error.

### Affected tests

Every polymarket v2 test depending on __postInit-initialised state
(orch admin/operator/owner roles, factory addresses, EIP-712 cache).
Blocks ~80% of v2 tests.

### 4c + 4d follow-up (deferred)

Both `test_fixed_arrays_in_storage` and `test_array_storage_index_boundary_test`
fail at runtime under puya 5.9 with the same root cause:

puya 5.9 changed how it lowers `box_extract` AWST nodes. Old (5.8):

  bytec "data"; <offset>; <length>; box_extract

emits a single `box_extract` opcode that asserts if the box doesn't
exist (Solidity-incompatible: returns the bytes if exists, reverts
otherwise).

New (5.9): wraps with a zero-default-on-missing fallback:

  pushint <full_logical_size>
  bzero                       ← `bzero(N)` default buffer
  bytec "data"; box_get       ← (value, exists)
  select                      ← box value if exists, else default
  <offset>; <length>; extract3 ← extract from chosen buffer

This is correct in spirit (matches EVM storage semantics where
uninitialized slots are zero). But it breaks for `box_extract` where
`<full_logical_size>` exceeds AVM's per-stack-value cap of 4096
bytes. The `bzero(N)` itself reverts at runtime.

In our two failing tests:
- `Data[2**10] data` (struct array): 1024 × 64 = 65536 byte logical
  buffer → `bzero(65536)` overflows the 4096 cap.
- `uint[] storageArray` followed by index access with bounds-check:
  similar dynamic-length codegen via `box_extract` with full-buffer
  default.

Puya-sol-side options:
- Implement multi-box storage for fixed-size STRUCT arrays (currently
  only scalar arrays use multi-box per `[[multi-box-storage]]`). This
  splits the 65536-byte buffer into 2 × 32768-byte boxes that fit
  individually and don't trigger the bzero(>4096).
- Emit the box-read via a custom IntrinsicCall chain (raw `box_get` +
  manual extract) that bypasses puya 5.9's box_extract lowering.

Puya-side options:
- Cap the `bzero(N)` default at 4096 bytes (AVM's stack-value max)
  and let the extract handle the boundary.
- Restore the old `box_extract` direct emission as an alternative
  lowering when the logical buffer size is large.

Deferred: multi-day work in either layer. Accept the -2 cost vs v290
for now.
