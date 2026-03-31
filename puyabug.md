# Puya Backend Bug: Constant Propagation Through Slot-Backed Subroutine Calls

## Summary

Puya's optimizer constant-propagates values through subroutine calls that modify slot-backed arrays (pass-by-reference). The optimizer sees `x[3] = 8; L.f(x); return x[3]` and folds `return x[3]` to `return 8`, not realizing that `L.f` modifies `x[3]` via the shared scratch slot.

## Minimal Reproducer

```solidity
// repro.sol
library L {
    function f(uint256[] memory _data) internal {
        _data[3] = 2;
    }
}

contract C {
    function f() public returns (uint256) {
        uint256[] memory x = new uint256[](7);
        x[3] = 8;
        L.f(x);
        return x[3]; // Should return 2, returns 8
    }
}
```

**Expected**: `f() → 2`
**Actual**: `f() → 8`

## Steps to Reproduce

```bash
# Compile to AWST + TEAL
puya-sol --source repro.sol --output-dir out --puya-path /path/to/puya

# Deploy and call f() — returns 8 instead of 2
```

## Analysis

### AWST is correct

The AWST correctly generates:
1. `AssignmentStatement`: `x = NewArray(7 elements)`
2. `ExpressionStatement`: `x[3] = IntegerConstant(8)` (IndexExpression assignment)
3. `ExpressionStatement`: `SubroutineCallExpression(L.f, args=[x])`
4. `ReturnStatement`: value = `IndexExpression(base=x, index=3)` ← reads from x, NOT constant 8

The subroutine `L.f` correctly takes `_data: ReferenceArray<biguint>` and assigns `_data[3] = 2`.

### TEAL shows the bug

The generated TEAL for `L.f` correctly uses scratch slot pass-by-reference:
```teal
L.f:
    proto 1 0
    frame_dig -1      // get slot number
    loads             // load array from slot
    pushbytes 0x...02 // value 2
    replace2 96       // replace at offset 96 (index 3 * 32)
    frame_dig -1      // get slot number again
    swap
    stores            // write modified array BACK to slot
    retsub
```

The caller `C.f` allocates a slot and calls `L.f`:
```teal
f:
    callsub _puya_lib.mem.new_slot  // allocate scratch slot
    dup
    pushbytes 0x...08...            // initial array with x[3]=8
    stores                          // store in slot
    callsub L.f                     // call L.f (modifies slot)
    pushbytes 0x151f7c75...08       // ← HARDCODED 8! Should read from slot
    log
    pushint 1
    return
```

After `callsub L.f`, the TEAL logs a **hardcoded return value of 8** instead of reading `x[3]` back from the scratch slot. The optimizer has replaced the `loads` + `extract` (read x[3] from slot) with the constant `8` from the earlier assignment.

### Root Cause

Puya's constant propagation / copy propagation pass sees:
1. `slot[offset 96] = 8`
2. `callsub L.f` (doesn't track that this modifies the slot)
3. `return slot[offset 96]` → optimized to `return 8`

The optimizer doesn't model subroutine calls as potentially modifying slot-backed data. Since `L.f` takes the slot number as a `uint64` parameter (not a typed reference), the optimizer doesn't know the slot contents may change.

## Impact

Affects any Solidity code that:
- Passes `memory` arrays/structs to internal functions
- The callee modifies elements
- The caller reads the modified elements after the call

This is a common pattern in Solidity libraries (`using L for T`).

## Affected Tests

From Solidity semantic test suite:
- `libraries/internal_library_function` — `f() → 2` but got 8
- `libraries/internal_library_function_calling_private` — same pattern
- `libraries/internal_library_function_return_var_size` — index out of bounds (related)
- ~8 other library tests with pass-by-reference modifications

## Suggested Fix

The constant propagation pass should treat `callsub` as a barrier that invalidates any slot-backed values. Specifically:
- When a subroutine takes a `uint64` parameter that could be a slot number, assume all slot contents are invalidated after the call
- Or: track which slots are passed to subroutines and only invalidate those

A simpler conservative fix: invalidate ALL slot-backed constants at every `callsub`.
