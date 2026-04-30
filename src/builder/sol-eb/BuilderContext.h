#pragma once

#include "awst/Node.h"
#include "awst/WType.h"

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
}

namespace puyasol::builder::eb
{

class InstanceBuilder;

/// Parameter remap entry: redirects an AST declaration to a unique variable name.
struct ParamRemap
{
	std::string name;
	awst::WType const* type;
};

/// Shared context owning all expression-builder state.
///
/// This is the central state object passed to all expression and statement
/// builders. It owns the per-translation mutable state (scope tables, pending
/// statement buffers, parameter remaps, etc.) and holds references to the
/// long-lived compiler services (TypeMapper, StorageMapper, function tables).
///
/// Built-instance dispatch and recursive expression building are exposed via
/// std::function callbacks — these are filled in by ExpressionBuilder while the
/// migration is in progress; once `ExpressionBuilder` is removed these will
/// become direct methods on this class.
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
	)
		: typeMapper(_typeMapper),
		  storageMapper(_storageMapper),
		  sourceFile(_sourceFile),
		  contractName(_contractName),
		  libraryFunctionIds(_libraryFunctionIds),
		  overloadedNames(_overloadedNames),
		  freeFunctionById(_freeFunctionById)
	{}

	BuilderContext(BuilderContext const&) = delete;
	BuilderContext& operator=(BuilderContext const&) = delete;
	BuilderContext(BuilderContext&&) = delete;
	BuilderContext& operator=(BuilderContext&&) = delete;

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
	std::map<int64_t, ParamRemap> paramRemaps;
	std::unordered_map<int64_t, std::string> superTargetNames;
	std::map<int64_t, std::shared_ptr<awst::Expression>> storageAliases;
	std::map<int64_t, std::shared_ptr<awst::Expression>> slotStorageRefs;
	std::map<int64_t, solidity::frontend::FunctionDefinition const*> funcPtrTargets;
	std::unordered_map<int64_t, unsigned long long> constantLocals;
	std::map<std::string, int64_t> varNameToId;
	std::map<int64_t, std::string> mappingKeyParams;
	bool inConstructor = false;
	bool inUncheckedBlock = false;

	/// Scratch slot for the `arr.push() = value` rewrite: SolAssignment
	/// stashes the RHS here before the LHS build, and SolArrayMethod's
	/// push() handler consumes it as the pushed element instead of a
	/// default value, returning the ArrayExtend expression directly.
	std::shared_ptr<awst::Expression> pendingArrayPushValue;

	// ── Recursive build callback (delegates back to ExpressionBuilder) ──
	/// Build a child Solidity expression into an AWST Expression.
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	// ── Binary/unary operation callbacks (delegates back to ExpressionBuilder) ──
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
	/// If the name is already taken by a different declaration in an outer scope,
	/// appends "__<id>" to make it unique.
	std::string resolveVarName(std::string const& _name, int64_t _declId)
	{
		// Honour explicit remaps (used by modifier inliner when the same
		// modifier — with its own local vars — is applied multiple times).
		auto remapIt = paramRemaps.find(_declId);
		if (remapIt != paramRemaps.end())
			return remapIt->second.name;

		auto it = varNameToId.find(_name);
		if (it != varNameToId.end() && it->second != _declId)
		{
			// Name is shadowed — use unique name
			std::string unique = _name + "__" + std::to_string(_declId);
			varNameToId[unique] = _declId;
			return unique;
		}
		varNameToId[_name] = _declId;
		return _name;
	}

	/// Look up the AWST variable name for a referenced declaration.
	std::string lookupVarName(std::string const& _name, int64_t _declId) const
	{
		std::string unique = _name + "__" + std::to_string(_declId);
		auto it = varNameToId.find(unique);
		if (it != varNameToId.end() && it->second == _declId)
			return unique;
		return _name;
	}

	// ── Scope guard (RAII) ──
	/// Snapshots and restores mutable scope state at scope boundaries
	/// (if/else branches, for/while bodies, blocks).
	class ScopeGuard
	{
	public:
		explicit ScopeGuard(BuilderContext& _ctx)
			: m_ctx(_ctx),
			  m_savedFuncPtrTargets(_ctx.funcPtrTargets),
			  m_savedStorageAliases(_ctx.storageAliases),
			  m_savedConstantLocals(_ctx.constantLocals),
			  m_savedVarNames(_ctx.varNameToId)
		{}
		~ScopeGuard()
		{
			m_ctx.funcPtrTargets = std::move(m_savedFuncPtrTargets);
			m_ctx.storageAliases = std::move(m_savedStorageAliases);
			m_ctx.constantLocals = std::move(m_savedConstantLocals);
			m_ctx.varNameToId = std::move(m_savedVarNames);
		}
		ScopeGuard(ScopeGuard const&) = delete;
		ScopeGuard& operator=(ScopeGuard const&) = delete;
	private:
		BuilderContext& m_ctx;
		std::map<int64_t, solidity::frontend::FunctionDefinition const*> m_savedFuncPtrTargets;
		std::map<int64_t, std::shared_ptr<awst::Expression>> m_savedStorageAliases;
		std::unordered_map<int64_t, unsigned long long> m_savedConstantLocals;
		std::map<std::string, int64_t> m_savedVarNames;
	};

	ScopeGuard pushScope() { return ScopeGuard(*this); }
};

} // namespace puyasol::builder::eb
