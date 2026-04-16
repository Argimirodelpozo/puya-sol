# Puya Bug: undefined biguint canary leaks to runtime

## Symptom
`b== arg 0 wanted bigint but got uint64` at runtime.

## Root cause
**File:** `puya/src/puya/teal/builder.py`, line 95-104

`visit_undefined` emits a cross-type canary: `Int(0)` for bytes vars,
`Byte(b"")` for uint64 vars. This is intentional — the wrong type
should cause a compile-time error if the undefined value is used before
being properly initialized.

However, in the `inline_assembly_for2` test, the SSA phi node for `x`
has its initial definition as `undefined` (from block@0), which feeds
into the loop condition on the first iteration. The optimizer doesn't
eliminate this path because:

1. `x#0 = φ(x#3 ← block@0, x#2 ← block@2)` where x#3 = undefined
2. On the first loop iteration, `x#0 = x#3 = Int(0)` (canary)
3. `sideeffect(2)` produces x#2 = 1 (biguint), but only for the NEXT phi
4. The condition `eq(i, sideeffect(2))` uses sideeffect's return value
   directly (not x#0), so the canary SHOULDN'T affect the comparison

BUT: the TEAL stack management puts x#0's `Int(0)` on the stack where
`b==` expects it to be a bytes value. The `sideeffect` return (bytes 0x01)
is compared with something that's `Int(0)` via `dig 1` — the dig reaches
the canary instead of the sideeffect result.

## Reproduction
`tests/solidity-semantic-tests/tests/inlineAssembly/inline_assembly_for2.sol`

## Analysis
The IR and MIR are type-correct:
- IR: `let x#0: biguint = undefined` → correctly typed
- MIR: `undefined` for x#0 → emitted as `Int(0)` canary
- f-store/f-load of `byte 0x` for b#0/d#0 are correct

The TEAL stack layout puts the canary `Int(0)` (for x-stack x#0) on the
actual stack. When the loop condition `dig 1` copies a value for `b==`,
it reaches the canary instead of the actual computed value.

## Possible fixes
1. Ensure the optimizer eliminates undefined phi paths that reach runtime
2. Use type-compatible canaries (Byte(b"") for bytes vars) — less useful
   for catching bugs but prevents runtime type errors
3. Insert an explicit assertion before any comparison that one of the
   operands might be undefined

---

# Puya Bug #2: signed mod/div constant folding produces wrong result

## Symptom
`(-(-t % 3)) * 5` where t=5 (int256) returns 1270 instead of 10.

## Reproduction
`tests/solidity-semantic-tests/tests/constantEvaluator/negative_fractional_mod.sol`

## Root cause
puya-sol's AWST is correct: uses buildSignedDivMod which computes
abs(a) % abs(b) then applies sign of dividend. The complete expression
tree is: `(negate(signedMod(negate(t), 3)) * 5) mod 2^256`. 

puya's constant folder evaluates this tree at compile time and produces
1270 (= 254 * 5) instead of 10 (= 2 * 5). The intermediate 254 = 0xFE
suggests the folder is truncating to 8 bits somewhere during the signed
mod computation, treating `int` as `int8` instead of `int256`.

## Affected tests
- constantEvaluator/negative_fractional_mod (0p/1f)
- Possibly other tests with signed mod/div on constants
