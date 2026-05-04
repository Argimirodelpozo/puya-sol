# puya bug report — `getbit` constant-fold IndexError

**Repro:** compile AAVE V4 `SpokeInstance.sol` through puya-sol →
puya. Expected: clean compile. Actual: `IndexError: list index out
of range` from puya's IR optimizer.

**Crash site:** `puya/src/puya/ir/optimize/intrinsic_simplification.py:601`

```python
elif intrinsic.op is AVMOp.getbit:
    match intrinsic.args:
        ...
        case [
            models.Value(atype=AVMType.bytes) as byte_arg,
            models.UInt64Constant(value=index),
        ] if (byte_const := _get_byte_constant(register_assignments, byte_arg)) is not None:
            binary_array = [
                x for xs in [bin(bb)[2:].zfill(8) for bb in byte_const.value] for x in xs
            ]
            the_bit = binary_array[index]   # ← IndexError when index >= len(binary_array)
            return models.UInt64Constant(source_location=op_loc, value=int(the_bit))
```

## Stack trace

```
File ".../puya/ir/optimize/intrinsic_simplification.py", line 601, in _try_fold_intrinsic
    the_bit = binary_array[index]
              ~~~~~~~~~~~~^^^^^^^
IndexError: list index out of range

The above exception caused:
  optimize_program_ir → _optimize → intrinsic_simplifier → _try_fold_intrinsic
critical: IndexError: list index out of range
```

The crash terminates the entire compile; no fallback to "leave the
op unfolded and let runtime handle it" — which is what the surrounding
folds do for analogous unfoldable cases (e.g. addition with non-
constant operands).

## How the out-of-range index arises

The AVM `getbit` opcode, when applied to a bytes value at a runtime
index, reverts at runtime if the index is past the bit length. So
the runtime IS bounds-checked. The optimizer's job is to fold *only
when the result is statically known* — and by symmetry, when the
index is statically out of range, the right answer is "don't fold;
let runtime revert".

The specific path that triggers the crash from puya-sol:

1. puya-sol's splitter / default-value codegen emits
   `BytesConstant({}, source_loc)` (= empty bytes literal) reinterpret-
   cast to a struct return type, as the placeholder for an
   uninitialized struct return / stubbed method body.
2. Later, a getter on that struct reads a packed bool field — puya
   lowers it to `getbit(struct_bytes, bit_offset)` where
   `bit_offset` is the field's byte position × 8.
3. If both args reach the optimizer as constants
   (`getbit(empty_bytes, k)` for `k >= 0`), the fold runs the
   binary-array unroll on an empty value, then indexes at `k` ≥ 0.
   IndexError.

## Suggested patch

```python
case [
    models.Value(atype=AVMType.bytes) as byte_arg,
    models.UInt64Constant(value=index),
] if (byte_const := _get_byte_constant(register_assignments, byte_arg)) is not None:
    binary_array = [
        x for xs in [bin(bb)[2:].zfill(8) for bb in byte_const.value] for x in xs
    ]
    if index >= len(binary_array):
        # out-of-range: defer to runtime (which will revert correctly)
        pass
    else:
        the_bit = binary_array[index]
        return models.UInt64Constant(source_location=op_loc, value=int(the_bit))
```

I've applied this locally as a stop-gap; SpokeInstance now compiles
to 22.8 KB. Same shape of patch should be considered for nearby
`setbit`/`extract`/`substring` constant-folds that haven't surfaced
in our workload yet but follow the same pattern.

## Companion observation: a possible deeper fix

The cleaner fix is on the producer side. puya-sol emits
`BytesConstant({}, ...)` reinterpret-cast as the default value for
struct return types — but a struct's "default" should be the
zero-filled bytes of its full encoded width (e.g., 32 bytes for a
struct of 32 packed bytes), not an empty literal that any subsequent
field-read indexes past. The bytes-shape mismatch is what makes
`getbit(empty, k)` reachable at all.

Two possible fixes, in order of preference:

1. **(producer side, cleaner) puya-sol** emits zero-filled bytes of
   the encoded struct's width when materialising default struct
   values.  This means `getbit` would be folded correctly to `0`
   instead of crashing.

2. **(consumer side, defensive) puya** defers folding when index is
   out of range (the patch above). Cheap, safe, doesn't require
   producers to be perfect.

Both would be useful — (2) covers all current and future producer
mistakes, (1) is the right semantics. We applied (2) in the puya
fork at `puya/src/puya/ir/optimize/intrinsic_simplification.py`.

## Other folds in the same file that may have the same shape

A quick audit:

- `extract` (substring of a bytes constant) — bounds check looks
  present, but worth re-verifying.
- `setbit` — same structure as getbit; same suggested patch.
- `getbyte` — same shape.

Each could have the same "index past constant length" case for the
same reason.
