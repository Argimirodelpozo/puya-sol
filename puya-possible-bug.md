# Puya Possible Bug: `extract3` with length=0 produces empty bytes

## Summary

`arc4_codecs.py` uses `factory.extract3(value, 2, 0)` to strip the 2-byte ARC4 length header from dynamic arrays. This emits the AVM `extract3` opcode with length=0 on the stack. However, `extract3` with length=0 returns **empty bytes**, not "extract to end".

The "extract to end when length=0" semantics only apply to the **immediate** `extract s l` opcode, not the stack-based `extract3`.

## Location

`puya/src/puya/ir/builder/aggregates/arc4_codecs.py`, around line 81 (in `_try_decode_bytes`):

```python
factory = OpFactory(context, loc)
stripped = factory.extract3(value, 2, 0, "decoded_array")
return stripped
```

## AVM Opcode Semantics

| Opcode | Form | length=0 behavior |
|--------|------|-------------------|
| `extract s l` | Immediate (compile-time constants) | Extracts from offset `s` **to end** |
| `extract3` | Stack (runtime values) | Extracts **0 bytes** (empty) |

Verified on AVM v10 (algod 4.4.1):
```teal
pushbytes 0x0003...   // 98 bytes
pushint 2
pushint 0
extract3               // → empty bytes (0 bytes), NOT 96 bytes
```

vs:
```teal
pushbytes 0x0003...   // 98 bytes
extract 2 0            // → 96 bytes (from offset 2 to end) ✓
```

## Impact

Any code path that hits the length-header stripping in `_try_decode_bytes` (dynamic arrays decoded from ARC4 format with `length_header=True` to a target with `length_header=False`) will produce an empty array instead of the actual array data.

## Existing Correct Usage

Line ~545 in the same file already uses the correct immediate form:
```python
return ir.Intrinsic(
    op=AVMOp.extract, immediates=[2, 0], args=[value], source_location=loc
)
```

## Suggested Fix

Replace:
```python
factory = OpFactory(context, loc)
stripped = factory.extract3(value, 2, 0, "decoded_array")
```

With:
```python
stripped = assign_intrinsic_op(
    context,
    target="decoded_array",
    op=AVMOp.extract,
    immediates=[2, 0],
    args=[value],
    source_location=loc,
)
```

## Workaround

The puya-sol frontend works around this by emitting the array data **without** the ARC4 length header in the AWST, so this code path is never hit. See the frontend fix in `ExpressionBuilder` / `FunctionCallBuilder` where dynamic array args are pre-stripped before being passed to subroutines.
