# Technical Debt

Tracking known limitations, shortcuts, and architectural improvements needed.

## 1. Simplify Context Management (Nested Scoped Contexts)

**Current state**: `ExpressionBuilder::ScopeGuard` is an RAII guard that snapshots/restores three mutable maps (`funcPtrTargets`, `storageAliases`, `constantLocals`) at block scope boundaries. It's applied in `SolBlock::toAwstBlock()`.

**Problem**: The current approach is fragile:
- Mutable state lives on `ExpressionBuilder` and is shared by reference via `BuilderContext`
- `ScopeGuard` only protects block-level scopes, not expression-level nesting
- Adding new context-dependent properties requires updating `ScopeGuard` manually
- `BuilderContext` mixes read-only references (libraryFunctionIds, overloadedNames) with mutable references (funcPtrTargets, storageAliases) — unclear ownership semantics

**Desired state**: A proper `ScopedContext` class that:
- Inherits from parent context (prototype chain / environment chain pattern)
- Automatically copies-on-write for all mutable properties
- Is created at every scope boundary (blocks, function bodies, modifier bodies, assembly blocks)
- Destroyed on scope exit, discarding local mutations
- New properties can be added without modifying scope machinery
- Clear separation between immutable context (type mapper, source file) and mutable context (variable tracking, storage aliases)

**Impact**: Correctness for any future context-dependent analysis (constant propagation, alias tracking, liveness). Current ScopeGuard works for simple cases but will silently break for complex control flow patterns (e.g., function pointer reassignment inside nested if/else with early returns).

## 2. ~~Implement Arbitrary Fallback~~ (PARTIALLY RESOLVED)

**Current state**: The test runner supports `()` bare calls by invoking the `"()void"` ABI method that our compiler emits for Solidity `fallback()` functions.

**Problem**: This only handles the simplest case. Solidity's fallback function has richer semantics:
- `fallback(bytes calldata) external returns (bytes memory)` — receives arbitrary calldata and can return data
- `receive() external payable` — separate from fallback, triggered on plain ether transfers (no data)
- Fallback is triggered for ANY call that doesn't match a known selector, not just empty calls
- The test format supports `(): data -> result` (fallback with data) and `(), 1 ether ->` (receive with value)

**What needs to change**:

### Compiler side
- Distinguish `fallback` vs `receive` in the ABI routing:
  - `receive()` → triggered when `NumAppArgs == 0` (bare call, no data)
  - `fallback(bytes)` → triggered when selector doesn't match any method (catch-all after `match` fails)
- Currently both are emitted as ABI methods, but the routing should be:
  ```
  if NumAppArgs == 0 → receive (or fallback if no receive)
  else → match selector → method | fallback
  ```
- The `err` after the `match` statement should route to fallback instead of aborting

### Test runner side
- `()` bare call → send with no app args (receive) OR with fallback selector
- `(): data` → send with raw data as app args (fallback with calldata)
- `(), N ether` → currently skipped as payable; could be supported with payment txn
- Handle fallback return data decoding
- For non-bare-call fallback testing: send a garbage selector to trigger fallback

### ARC4 compatibility
- The current `"()void"` method name is a workaround; a proper implementation would use ARC4 bare method handling or a dedicated fallback routing path in the approval program

## 3. Review Modified Test: `tests/solidity-semantic-tests/tests/userDefinedValueType/ownable.sol`

**What was changed**: The test's assertions were modified because EVM and Algorand use different address formats. The original test hardcodes `0x1212...12` as the expected `msg.sender` (EVM test runner address). On Algorand, `msg.sender` is a 32-byte Algorand address that doesn't match.

**What was skipped**:
- `owner() -> 0x1212...12` — initial owner address check (address is runtime-dependent)
- `setOwner(address): 0x1212...12 ->` — transfers ownership to an address the caller can't use for subsequent `onlyOwner` calls

**What is still tested**:
- `renounceOwnership() ->` — caller is initial owner, succeeds
- `owner() -> 0` — owner is now zero after renounce
- `setOwner(address): 0x1212...12 -> FAILURE` — fails because owner is zero

**Review needed**: Once we have proper address mapping (EVM address ↔ Algorand address), this test should be restored to its original form. The original assertions are preserved in `# ... #` comments in the test file.

## 4. Modifier-as-Subroutine (Solidity's Approach)

**Current state**: Modifiers are inlined into the function body. The `_;` placeholder is replaced with the function body inline. Local variables in the modifier share the same AWST namespace across all invocations.

**Problem**: When the same modifier is invoked multiple times (`mod(2) mod(5) mod(x)`), local variables alias. Post-body code like `a -= b; assert(b == x)` uses the wrong `b` value.

**How Solidity does it**: Each modifier invocation becomes a **separate Yul function** with its own scope. The `_;` placeholder becomes a call to the next function in the chain. See `IRGenerator::generateModifier()` in `solidity/libsolidity/codegen/ir/IRGenerator.cpp:385`. Key patterns:
- `m_context.resetLocalVariables()` — fresh scope per modifier function
- `addLocalVariable(*varDecl)` — unique Yul names via `newYulVariable()`
- Chain: `mod_2(retParams, params)` calls `mod_5(retParams, params)` which calls `f_inner(retParams, params)`

**Fix**: Emit each modifier invocation as a separate AWST Subroutine. The function body becomes the innermost subroutine. Each modifier subroutine takes return params + function params, evaluates modifier args, runs modifier body, and calls the next subroutine at `_;`.

**Impact**: Fixes `function_modifier_multiple_times_local_vars` and potentially other modifier tests (13 total). Also fixes modifier local variable scoping generally.

## 5. C99 Variable Scoping (Unique Variable Names)

**Current state**: All local variables use their Solidity name as the AWST VarExpression name. Two variables with the same name in nested scopes (shadowing) produce the same AWST name, causing them to alias.

**Problem**: `{ x = 3; uint x; x = 4; } return x;` returns 4 instead of 3 because inner and outer `x` share the same AWST name.

**Fix approach**: Use `name_declId` (e.g., `x_42`) for all local variable declarations and references. The AST declaration ID guarantees uniqueness. Requires updating:
- `SolVariableDeclaration.cpp` — variable declaration names
- `SolIdentifier.cpp` — variable reference names
- `ContractBuilder.cpp` — named return parameter synthesis (lines 1989, 2030, 2042, 2438-2439 and more)
- Any other code that creates VarExpression with raw Solidity names

**Impact**: Fixes `scoping/c99_scoping_activation` and potentially modifier local variable scoping issues.
