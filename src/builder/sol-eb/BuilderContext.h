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
class BuilderContext
{
public:
	BuilderContext(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		std::unordered_map<std::string, std::string> const& _libraryFunctionIds,
		std::unordered_set<std::string> const& _overloadedNames,
		std::unordered_map<int64_t, std::string> const& _freeFunctionById
	);

	~BuilderContext();

	BuilderContext(BuilderContext const&) = delete;
	BuilderContext& operator=(BuilderContext const&) = delete;
	BuilderContext(BuilderContext&&) = delete;
	BuilderContext& operator=(BuilderContext&&) = delete;

	/// Build an AWST expression from a Solidity expression. Primary entry point.
	std::shared_ptr<awst::Expression> build(solidity::frontend::Expression const& _expr);

	/// Consume any pending statements generated during expression translation.
	std::vector<std::shared_ptr<awst::Statement>> takePending();

	/// Consume any pre-pending statements (must execute before the expression).
	std::vector<std::shared_ptr<awst::Statement>> takePrePending();

	/// Owned type-builder registry — populated on construction.
	std::unique_ptr<BuilderRegistry> registry;

	// ── Compiler services (external, by reference) ──
	TypeMapper& typeMapper;
	StorageMapper& storageMapper;
	/// Transient storage manager — non-null only when the current contract
	/// has transient state variables. Used to route reads/writes of
	/// `transient` state vars to a per-transaction blob.
	TransientStorage* transientStorage = nullptr;
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

	// ── Side-effect statement buffers (owned) ──
	std::vector<std::shared_ptr<awst::Statement>> pendingStatements;
	std::vector<std::shared_ptr<awst::Statement>> prePendingStatements;

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
		ScopePush(BuilderContext& _ctx, sol_ast::Context* _new)
			: m_ctx(_ctx), m_prev(_ctx.currentScope)
		{
			m_ctx.currentScope = _new;
		}
		~ScopePush() { m_ctx.currentScope = m_prev; }
		ScopePush(ScopePush const&) = delete;
		ScopePush& operator=(ScopePush const&) = delete;
	private:
		BuilderContext& m_ctx;
		sol_ast::Context* m_prev;
	};

	[[nodiscard]] ScopePush pushScopeRaii(sol_ast::Context* _scope)
	{
		return ScopePush(*this, _scope);
	}

	/// Convenience: true if the innermost scope (or any of its ancestors)
	/// is inside an `unchecked { }` block. Resolved by chain walk through
	/// `currentScope`. Returns false before any scope is pushed.
	bool isUnchecked() const;

	/// Look up a storage-pointer alias by AST decl ID. Walks `currentScope`
	/// up the parent chain; returns nullptr if no alias is bound.
	std::shared_ptr<awst::Expression> findStorageAlias(int64_t _declId) const;

	/// Bind a storage-pointer alias in the innermost enclosing block.
	/// Lifetime is the block's lifetime — the binding is discarded when
	/// the block ends (its BlockContext destructs).
	void setStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr);

	/// Look up a function-pointer target by AST decl ID.
	solidity::frontend::FunctionDefinition const* findFuncPtrTarget(
		int64_t _declId
	) const;

	/// Bind a function-pointer target in the innermost enclosing block.
	void setFuncPtrTarget(
		int64_t _declId,
		solidity::frontend::FunctionDefinition const* _target
	);

	/// Erase a function-pointer target binding from the innermost enclosing
	/// block where it exists. No-op if not bound.
	void eraseFuncPtrTarget(int64_t _declId);

	/// Look up a folded compile-time constant value for a local. Returns 0
	/// if the local isn't constant-folded (callers check `> 0`).
	unsigned long long findConstantLocal(int64_t _declId) const;

	/// Bind a constant value for a local in the innermost enclosing block.
	void setConstantLocal(int64_t _declId, unsigned long long _value);

	/// Look up a slot-based storage ref by AST decl ID.
	std::shared_ptr<awst::Expression> findSlotStorageRef(int64_t _declId) const;

	/// Bind a slot-based storage ref in the innermost enclosing block.
	void setSlotStorageRef(int64_t _declId, std::shared_ptr<awst::Expression> _expr);

	/// True if the innermost enclosing function is a constructor body.
	bool isInConstructor() const;

	/// Set/clear the constructor flag on the innermost enclosing function.
	void setInConstructor(bool _flag);

	/// Look up a mapping-storage-pointer param's holder name.
	std::string findMappingKeyParam(int64_t _declId) const;

	/// Returns true if the given decl is bound as a mapping-storage param
	/// in any enclosing function. (Convenience for `count()` callers.)
	bool hasMappingKeyParam(int64_t _declId) const;

	/// Bind a mapping-storage-pointer param on the innermost enclosing
	/// function.
	void setMappingKeyParam(int64_t _declId, std::string _name);

	/// Modifier-inliner param remap accessors. Bindings live on the
	/// innermost enclosing FunctionContext.
	sol_ast::ParamRemap const* findParamRemap(int64_t _declId) const;
	void setParamRemap(int64_t _declId, sol_ast::ParamRemap _remap);
	void eraseParamRemap(int64_t _declId);

	/// `super.X()` MRO resolution accessors. Bindings live on the
	/// innermost enclosing TranslationContext.
	std::string findSuperTarget(int64_t _declId) const;
	void setSuperTarget(int64_t _declId, std::string _name);
	void clearSuperTargets();
	std::unordered_map<int64_t, std::string> const& allSuperTargets() const;

	/// Scratch slot for the `arr.push() = value` rewrite: SolAssignment
	/// stashes the RHS here before the LHS build, and SolArrayMethod's
	/// push() handler consumes it as the pushed element instead of a
	/// default value, returning the ArrayExtend expression directly.
	std::shared_ptr<awst::Expression> pendingArrayPushValue;

	// ── Recursive build callback (delegates to BuilderContext::build) ──
	/// Build a child Solidity expression into an AWST Expression.
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	// ── Binary/unary operation callbacks (delegates to BuilderContext::build) ──
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

	// ── Variable-name resolution (handles shadowing) ──
	/// Get the AWST variable name for a declaration, handling shadowing.
	/// If the name is already taken by a different declaration in an outer
	/// scope, appends "__<id>" to make it unique. Bindings are inserted
	/// into the innermost enclosing BlockContext.
	std::string resolveVarName(std::string const& _name, int64_t _declId);

	/// Look up the AWST variable name for a referenced declaration.
	/// Returns `_name__declId` if such a unique-name binding exists in
	/// any enclosing block, otherwise the bare `_name`.
	std::string lookupVarName(std::string const& _name, int64_t _declId) const;

};

} // namespace puyasol::builder::eb
