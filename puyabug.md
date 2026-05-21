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
