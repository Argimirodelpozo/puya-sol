#pragma once

#include "awst/Node.h"
#include "awst/WType.h"
#include "builder/sol-ast/Context.h"

#include <libsolidity/ast/Types.h>
#include <liblangutil/Token.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace solidity::frontend
{
class Expression;
class FunctionDefinition;
class Declaration;
class ContractDefinition;
}

namespace puyasol::builder
{
class TypeMapper;
class StorageMapper;
class StorageBackend;
class TransientStorage;
namespace sol_ast {
class Context;
struct ParamRemap;
}
}

namespace puyasol::builder::eb
{

class InstanceBuilder;
class BuilderRegistry;

// `ParamRemap` is now defined in `sol_ast/Context.h` since it's owned by
// `FunctionContext`. Re-exported as `eb::ParamRemap` for legacy callers
// (e.g. ModifierInliner) that use the qualified name.
using ParamRemap = sol_ast::ParamRemap;

/// Shared context owning all expression-builder state.
///
/// This is the central state object passed to all expression and statement
/// builders. It owns the per-translation mutable state (scope tables, pending
/// statement buffers, parameter remaps, etc.) and holds references to the
/// long-lived compiler services (TypeMapper, StorageMapper, function tables).
/// It also owns the type-builder registry used by sol-eb dispatch.
///
/// Recursive expression building, fallback binary-op construction, and
/// type-builder dispatch are exposed via std::function callbacks wired up in
/// the constructor; the field-and-callback layout is preserved as the public
/// surface that sol-ast wrappers consume.
class ContractContext
{
public:
	ContractContext(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		std::unordered_map<std::string, std::string> const& _libraryFunctionIds,
		std::unordered_set<std::string> const& _overloadedNames,
		std::unordered_map<int64_t, std::string> const& _freeFunctionById
	);

	~ContractContext();

	ContractContext(ContractContext const&) = delete;
	ContractContext& operator=(ContractContext const&) = delete;
	ContractContext(ContractContext&&) = delete;
	ContractContext& operator=(ContractContext&&) = delete;

	/// Build an AWST expression from a Solidity expression. Primary entry point.
	std::shared_ptr<awst::Expression> build(solidity::frontend::Expression const& _expr);

	/// Consume any pending statements generated during expression translation.
	std::vector<std::shared_ptr<awst::Statement>> takePending();

	/// Consume any pre-pending statements (must execute before the expression).
	std::vector<std::shared_ptr<awst::Statement>> takePrePending();

	/// Drain BOTH pre-pending and pending into `_out` in execution order
	/// (pre-pending first). The vast majority of statement-builder sites
	/// flush both together right before returning their result vector;
	/// keeping them as one call removes the four-line two-loop boilerplate.
	void appendPendingTo(std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Owned type-builder registry — populated on construction.
	std::unique_ptr<BuilderRegistry> registry;

	// ── Compiler services (external, by reference) ──
	TypeMapper& typeMapper;
	StorageMapper& storageMapper;
	/// Transient storage manager — non-null only when the current contract
	/// has transient state variables. Used to route reads/writes of
	/// `transient` state vars to a per-transaction blob.
	TransientStorage* transientStorage = nullptr;
	/// Unified dispatch over AppGlobal / Box / Transient backends. Use
	/// `storageBackend->emitReadForVar(decl, ...)` / `emitWriteForVar` for
	/// any state-var access driven by a VariableDeclaration; falls back to
	/// the right backend internally. Populated alongside transientStorage
	/// in ContractBuilder.
	StorageBackend* storageBackend = nullptr;
	std::string const& sourceFile;
	std::string const& contractName;
	/// The contract currently being built — used e.g. to look up the
	/// contract's fallback function signature for self-call emulation.
	/// May be nullptr during free-function translation.
	solidity::frontend::ContractDefinition const* currentContract = nullptr;

	// ── Function resolution tables (external, by reference) ──
	std::unordered_map<std::string, std::string> const& libraryFunctionIds;
	std::unordered_set<std::string> const& overloadedNames;
	std::unordered_map<int64_t, std::string> const& freeFunctionById;
	/// AST IDs of library functions promoted to per-contract internal methods.
	/// Maps funcDef.id() → the synthesized method name on the current contract.
	/// CallResolver consults this first, returning InstanceMethodTarget instead
	/// of SubroutineID when the funcDef is internalized.
	std::unordered_map<int64_t, std::string> internalizedLibFuncNames;

	// ── Side-effect statement buffers (owned) ──
	std::vector<std::shared_ptr<awst::Statement>> pendingStatements;
	std::vector<std::shared_ptr<awst::Statement>> prePendingStatements;

	/// Queue an expression as a side-effect statement that will run as part
	/// of the enclosing statement. Wraps `expr` in an ExpressionStatement
	/// and appends to `pendingStatements`. Use for "do this after the
	/// current expression evaluates" semantics.
	void queueStmt(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		pendingStatements.push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Queue a statement directly onto `pendingStatements` (no
	/// ExpressionStatement wrapper). Use when the caller already has a
	/// Statement (e.g. AssignmentStatement, ExpressionStatement).
	void queuePending(std::shared_ptr<awst::Statement> stmt)
	{
		pendingStatements.push_back(std::move(stmt));
	}

	/// Queue an expression to run BEFORE the enclosing statement
	/// (prePending). Use for "do this before the LHS evaluates" semantics
	/// — e.g. `arr.push().field = v` needs the extend before the field
	/// write reads `length-1`.
	void queuePreStmt(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		prePendingStatements.push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Like `queuePending` but for prePendingStatements — append a
	/// Statement directly to the buffer that runs BEFORE the enclosing
	/// statement.
	void queuePrePending(std::shared_ptr<awst::Statement> stmt)
	{
		prePendingStatements.push_back(std::move(stmt));
	}

	// ── Per-translation scope state (owned) ──

	/// Innermost active scope. Updated on entry to each
	/// TranslationContext / FunctionContext / BlockContext via
	/// `pushScope`; descendants' parent chain reaches the same nodes.
	/// Null before the first scope is pushed (only happens during very
	/// early ContractBuilder setup).
	sol_ast::Context* currentScope = nullptr;

	/// RAII helper: stash the previous scope on construct, restore on
	/// destruct. Use via `auto guard = ctx.pushScope(&someContext);`.
	class ScopePush
	{
	public:
		ScopePush(ContractContext& _ctx, sol_ast::Context* _new)
			: m_ctx(_ctx), m_prev(_ctx.currentScope)
		{
			m_ctx.currentScope = _new;
		}
		~ScopePush() { m_ctx.currentScope = m_prev; }
		ScopePush(ScopePush const&) = delete;
		ScopePush& operator=(ScopePush const&) = delete;
	private:
		ContractContext& m_ctx;
		sol_ast::Context* m_prev;
	};

	[[nodiscard]] ScopePush pushScopeRaii(sol_ast::Context* _scope)
	{
		return ScopePush(*this, _scope);
	}

	// Scope-bound state accessors all moved onto `sol_ast::Context`
	// itself (Context.h). Visitors / builders use `m_scope.findX()` etc.
	// directly; helpers (ApprovalProgramBuilder, SuperCallResolution,
	// ModifierInliner, AWSTBuilder freestanding-subroutine path) use
	// `m_tr->setX()` / local fnCtx / blk. The bridge methods that
	// previously delegated through `currentScope` have been deleted.
	//
	// `currentScope` itself is still alive as plumbing — `pushScopeRaii`
	// updates it on scope entry, and `SolExpression`/`NodeBuilder`
	// constructors capture it as their own `m_scope` reference. Once
	// scope is threaded explicitly through `buildExpr`/`buildBinaryOp`/
	// the InstanceBuilder factory, that field can go too.

	/// Scratch slot for the `arr.push() = value` rewrite: SolAssignment
	/// stashes the RHS here before the LHS build, and SolArrayMethod's
	/// push() handler consumes it as the pushed element instead of a
	/// default value, returning the ArrayExtend expression directly.
	std::shared_ptr<awst::Expression> pendingArrayPushValue;

	// ── Recursive build callback (delegates to ContractContext::build) ──
	/// Build a child Solidity expression into an AWST Expression.
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	// ── Binary/unary operation callbacks (delegates to ContractContext::build) ──
	/// Build a binary operation from already-resolved operands (fallback when sol-eb builders don't handle it).
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Token, std::shared_ptr<awst::Expression>,
		std::shared_ptr<awst::Expression>, awst::WType const*,
		awst::SourceLocation const&)> buildBinaryOp;

	// ── Builder factory callback ──
	/// Get a type-specific InstanceBuilder for an already-resolved expression.
	/// Returns nullptr if no builder is registered for the Solidity type.
	std::function<std::unique_ptr<InstanceBuilder>(
		solidity::frontend::Type const*, std::shared_ptr<awst::Expression>)> builderForInstance;

	// ── Source location helper ──
	/// Create an AWST SourceLocation from file + offset range.
	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
		return loc;
	}

};

} // namespace puyasol::builder::eb

namespace puyasol::builder
{
// `ContractContext` is conceptually a per-contract translation context,
// not specifically an expression-builder helper. Hoist the name into the
// enclosing `puyasol::builder` namespace via a `using` alias so callers
// outside `eb/` can spell it `builder::ContractContext`. The class itself
// stays in `eb` because most of its dependencies (BuilderRegistry,
// InstanceBuilder) genuinely belong there.
using ContractContext = eb::ContractContext;
}
