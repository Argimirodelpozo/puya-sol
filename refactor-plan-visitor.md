# Polymorphic Visitor Refactor — Plan for Review

**Baseline**: v137, 1004/1322 semantic tests pass (committed).
**Goal**: Replace the dynamic_cast dispatch chains with a polymorphic visitor model built on Solidity's existing `ASTConstVisitor` virtual dispatch. Fully recursive, idiomatic, no behavior changes.

## Current state (the problem)

Three layers of dynamic_cast dispatch:

1. **Sol-AST expression dispatch** — `SolExpressionDispatch.cpp` (14 `dynamic_cast` checks, linear chain) maps `solidity::frontend::Expression&` → one of 13 `SolExpression` subclasses.
2. **Sol-AST factory dispatch** — `SolExpressionFactory.cpp` (27 `dynamic_cast`s) routes FunctionCall by `FunctionType::Kind` (a clean switch) and MemberAccess by a mix of annotation checks and base-type casts. This is partly type-dispatch, partly annotation-dispatch.
3. **Sol-AST statement dispatch** — `SolBlock.cpp::dispatchStatementImpl` (12 `dynamic_cast`s) maps `Statement&` → `SolStatement` subclasses.

Plus an orthogonal AWST-side issue: `AWSTBuilder.cpp` has `dynamic_cast` chains walking the **output** AWST (not Solidity AST) — structural cleanup there is a different problem with different trade-offs.

## Proposed design

### Core idea: lean on Solidity's `ASTConstVisitor`

Solidity already provides `virtual bool visit(X&)` for every concrete AST node. Instead of a hand-rolled dispatcher with 14 casts, we make our dispatcher a subclass of `ASTConstVisitor` — one virtual-dispatch call replaces the whole chain.

```cpp
class ExpressionDispatcher : public solidity::frontend::ASTConstVisitor {
public:
    explicit ExpressionDispatcher(eb::BuilderContext& ctx) : m_ctx(ctx) {}
    std::shared_ptr<awst::Expression> build(Expression const& e) {
        m_result.reset();
        e.accept(*this);
        return std::move(m_result);
    }
    bool visit(Literal const& n) override            { m_result = SolLiteral(m_ctx, n).toAwst(); return false; }
    bool visit(Identifier const& n) override         { m_result = SolIdentifier(m_ctx, n).toAwst(); return false; }
    bool visit(BinaryOperation const& n) override    { m_result = SolBinaryOperation(m_ctx, n).toAwst(); return false; }
    bool visit(UnaryOperation const& n) override     { m_result = SolUnaryOperation(m_ctx, n).toAwst(); return false; }
    bool visit(Conditional const& n) override        { m_result = SolConditional(m_ctx, n).toAwst(); return false; }
    bool visit(Assignment const& n) override         { m_result = SolAssignment(m_ctx, n).toAwst(); return false; }
    bool visit(IndexAccess const& n) override        { m_result = SolIndexAccess(m_ctx, n).toAwst(); return false; }
    bool visit(IndexRangeAccess const& n) override   { m_result = SolIndexRangeAccess(m_ctx, n).toAwst(); return false; }
    bool visit(TupleExpression const& n) override    { m_result = SolTupleExpression(m_ctx, n).toAwst(); return false; }
    bool visit(FunctionCall const& n) override       { m_result = dispatchFunctionCall(n); return false; }
    bool visit(MemberAccess const& n) override       { m_result = dispatchMemberAccess(n); return false; }
    bool visit(FunctionCallOptions const& n) override{ m_result = build(n.expression()); /* + warn */ return false; }
    bool visit(ElementaryTypeNameExpression const&) override { m_result = makeVoid(); return false; }
private:
    eb::BuilderContext& m_ctx;
    std::shared_ptr<awst::Expression> m_result;
    // ... dispatchFunctionCall / dispatchMemberAccess same as factory today
};
```

- Returning `false` from every `visit` prevents Solidity from descending into children — we're a builder, not a traversal. Children are built recursively via `build()` inside each handler's `toAwst()`, which already happens today.
- `build()` is re-entrant (every handler's `toAwst()` eventually calls it for sub-expressions), so we allocate a fresh dispatcher per recursion via `m_ctx.buildExpr()`. No shared mutable state leakage.
- **One-line idiomatic dispatch replaces 14 `dynamic_cast` checks.**

### Statement dispatch: same pattern

Replace `dispatchStatementImpl` with a `StatementDispatcher : ASTConstVisitor` returning `std::vector<std::shared_ptr<awst::Statement>>`. Twelve `visit(...)` overloads covering ExpressionStatement, Return, RevertStatement, EmitStatement, VariableDeclarationStatement, IfStatement, WhileStatement, ForStatement, InlineAssembly, Continue, Break, PlaceholderStatement, TryStatement, Block. Nested-Block inlining in `SolBlock::toAwstBlock` stays.

### Factory-level dispatch: keep what's already clean

- **FunctionCall `Kind` dispatch** is already a `switch` on an enum — *not* dynamic_cast chains. It's fine. Leave it.
- **MemberAccess routing** mixes annotation checks (isConstant, isStateVariable, referencedDeclaration category) with type casts (MagicType, TypeType, FunctionType, ContractType, AddressType). These are annotation-driven, not AST-structure-driven, so the visitor pattern doesn't help. Keep the existing ordered-check form, but split each routing decision into a small named predicate (`isIntrinsic`, `isEnumValue`, `isSelector`, `isTypeMeta`, `isFunctionPointer`, …) to make the sequence readable.

### Sol-AST → still use the handler classes

The handler classes (`SolLiteral`, `SolBinaryOperation`, etc.) are the right unit of encapsulation and are working fine. The refactor only replaces the *dispatcher*; it does not touch 38 handler files. That keeps blast radius small.

### Out of scope for this refactor

- AWST-side dynamic_casts in `AWSTBuilder.cpp`. Those walk the output AWST to look for patterns like `if (lhs) assert(rhs)`. Touching them is a separate project (requires adding a visitor on the AWST types, which are serialization shims — much more invasive).
- Rewriting handler internals to traverse polymorphically. Current `toAwst()` already recurses via `m_ctx.buildExpr()`, which re-enters the dispatcher. That's "fully recursive" already.
- Intrinsics / IntrinsicMapper / TypeMapper. Different problem.

## Migration strategy

Incremental, compile-and-semantic-test after each step so we never accumulate regressions.

### Phase 1: Expression dispatcher (1 commit)
1. Add `ExpressionDispatcher` class next to `SolExpressionDispatch.cpp` (or replace the file contents).
2. Keep `buildExpression(ctx, expr)` free function as the entry point — it now constructs and runs an `ExpressionDispatcher`.
3. `SolExpressionFactory::createFunctionCall/createMemberAccess` become private helpers of the dispatcher (or stay as free functions; either works).
4. Compile, run semantic suite, expect **1004/1322 pass, zero regressions**.

### Phase 2: Statement dispatcher (1 commit)
5. Replace `dispatchStatementImpl` in `SolBlock.cpp` with a `StatementDispatcher : ASTConstVisitor`.
6. `buildStatement` / `buildBlock` wire into it.
7. Compile, run semantic suite, **1004/1322**.

### Phase 3: MemberAccess predicate split (optional, 1 commit)
8. Extract `isIntrinsic`, `isEnumValue`, etc. from the body of `createMemberAccess` into named inline predicates. Pure readability pass — should not change dispatch order.
9. Compile, run semantic suite, **1004/1322**.

### Phase 4: Final cleanup (1 commit)
10. Delete `SolExpressionDispatch.h` wrapper if now trivially redundant. Check for dead includes.
11. Update CLAUDE.md / memory with new architecture pointer.

Rollback: every phase is one commit. `git revert` unwinds any phase independently.

## Expected touch points (file list)

- **Rewrite**: `src/builder/sol-ast/SolExpressionDispatch.cpp`, `SolExpressionDispatch.h`
- **Modify**: `src/builder/sol-ast/SolExpressionFactory.cpp` (becomes helper of dispatcher; possibly absorbed)
- **Modify**: `src/builder/sol-ast/stmts/SolBlock.cpp` (replace `dispatchStatementImpl`)
- **Include additions**: each dispatcher TU needs all 38 handler headers (already the case today)
- **No changes**: the 38 handler classes under `exprs/`, `calls/`, `members/`, `stmts/`

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Virtual-dispatch through `ASTConstVisitor` misses a case the dynamic_cast chain caught | Keep the final `else` unhandled-warning path; run semantic suite after each phase. |
| `ASTConstVisitor` default `visit()` returns `true`, causing unwanted child descent | Every override returns `false`. Default we never reach. |
| Re-entrancy via `m_ctx.buildExpr()` allocates new dispatcher per recursion — perf cost | One allocation per expression. No observed impact; dominated by downstream puya compile times. |
| Handler class header include explosion in the dispatcher TU | Not new — already the case today. |

## What "fully recursive, polymorphic" means here

- **Polymorphic**: dispatch via Solidity's `virtual bool visit(X&)` overloads, not our `dynamic_cast` chain. Compile-time resolved by the vtable.
- **Recursive**: every handler's `toAwst()` that needs sub-expressions calls `m_ctx.buildExpr(child)`, which re-enters a dispatcher. Already the case today — this refactor preserves it.

## Not proposed (and why)

- **Double-dispatch on our own class hierarchy** (adding `virtual accept(Visitor&)` to our `SolExpression` base): requires registering every handler at construction time, but we *construct* the handlers from dispatch — chicken-and-egg. The Solidity visitor solves this because it dispatches on the *input* `solidity::frontend::Expression`, not on our wrapper.
- **Type-index registry (`unordered_map<type_index, BuilderFn>`)**: works, but replaces compile-time vtable dispatch with a runtime hash lookup. Strictly worse than leaning on Solidity's ASTConstVisitor.
- **Replacing the annotation-driven MemberAccess routing with polymorphism**: annotations are not structural; the check sequence (is constant? is state var? is magic type? is function pointer?) has implicit precedence. A visitor can't express that.

---

**Awaiting your review.** If the plan looks good, I'll execute Phase 1 first and confirm **1004/1322** before moving on. If you want a different scope (e.g., include the AWSTBuilder.cpp AWST-side dispatch, or go further and eliminate the handler classes altogether), say so and I'll revise before starting.
