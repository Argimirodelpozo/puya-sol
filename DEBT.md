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

## 2. Implement Arbitrary Fallback (Not Just Bare Calls)

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
