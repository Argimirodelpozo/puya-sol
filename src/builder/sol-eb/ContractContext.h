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

// Re-exported from sol_ast::Context for legacy callers (e.g. ModifierInliner).
using ParamRemap = sol_ast::ParamRemap;

/// Central context for expression/statement builders: owns pending-statement buffers,
/// references compiler services (TypeMapper, StorageMapper), and holds the builder
/// registry. Callbacks (buildExpr, buildBinaryOp, builderForInstance) are wired
/// in the constructor and consumed by sol-ast wrappers.
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

		/// Drain pre-pending then pending into `_out` (execution order).
	void appendPendingTo(std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Owned type-builder registry — populated on construction.
	std::unique_ptr<BuilderRegistry> registry;

	// ── Compiler services (external, by reference) ──
	TypeMapper& typeMapper;
	StorageMapper& storageMapper;
	/// Non-null when the contract has transient state vars.
	TransientStorage* transientStorage = nullptr;
	/// Unified AppGlobal/Box/Transient dispatch; populated alongside transientStorage.
	StorageBackend* storageBackend = nullptr;
	std::string const& sourceFile;
	std::string const& contractName;
	/// Current contract (nullptr during free-function translation).
	solidity::frontend::ContractDefinition const* currentContract = nullptr;

	// ── Function resolution tables (external, by reference) ──
	std::unordered_map<std::string, std::string> const& libraryFunctionIds;
	std::unordered_set<std::string> const& overloadedNames;
	std::unordered_map<int64_t, std::string> const& freeFunctionById;
	/// funcDef.id() → synthesized method name; CallResolver returns InstanceMethodTarget
	/// instead of SubroutineID when the funcDef appears here.
	std::unordered_map<int64_t, std::string> internalizedLibFuncNames;

	// ── Side-effect statement buffers (owned) ──
	std::vector<std::shared_ptr<awst::Statement>> pendingStatements;
	std::vector<std::shared_ptr<awst::Statement>> prePendingStatements;

	/// Append expr as a post-statement side effect (after current expression evaluates).
	void queueStmt(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		pendingStatements.push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Append a Statement directly to pendingStatements (no ExpressionStatement wrapper).
	void queuePending(std::shared_ptr<awst::Statement> stmt)
	{
		pendingStatements.push_back(std::move(stmt));
	}

	/// Append expr as a pre-statement side effect (before LHS evaluates, e.g. arr.push()).
	void queuePreStmt(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		prePendingStatements.push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Append a Statement directly to prePendingStatements.
	void queuePrePending(std::shared_ptr<awst::Statement> stmt)
	{
		prePendingStatements.push_back(std::move(stmt));
	}

	// ── OperandPlan: scoped effect sequencing (fable-review item 7) ──
	// Pre-statements (overflow/zero asserts, `**` loops, box materializations,
	// inner-txn side effects) are normally pushed to the flat prePendingStatements
	// list and flushed UNCONDITIONALLY before the current statement. That is wrong
	// for an operand that only executes CONDITIONALLY — a ternary branch, a
	// short-circuit RHS — where its pre-statements must be gated behind the same
	// condition. Every such site used to hand-roll a snapshot/extract/erase of the
	// list (the C1 bug class: short-circuit RHS hoist 5a1f5810ad, ternary operand
	// SE cd9d91ccfa). These two primitives own that invariant in one place.

	/// Depth of CONDITIONALLY-EXECUTED translation regions (if/else branches,
	/// loop bodies, ternary/short-circuit arms). Compile-time-only state
	/// mutations — storage-pointer rebinds resolved via setStorageAlias — are
	/// UNSOUND inside one (the rebind would apply unconditionally to all
	/// later uses); producers of such state must fail loud when depth > 0.
	int conditionalDepth = 0;

	/// RAII marker for a conditionally-executed translation region.
	class ConditionalRegion
	{
	public:
		explicit ConditionalRegion(ContractContext& _ctx): m_ctx(_ctx) { ++m_ctx.conditionalDepth; }
		~ConditionalRegion() { --m_ctx.conditionalDepth; }
		ConditionalRegion(ConditionalRegion const&) = delete;
		ConditionalRegion& operator=(ConditionalRegion const&) = delete;
	private:
		ContractContext& m_ctx;
	};

	/// Build an operand via `_build` (returns its expression, may push
	/// pre-statements), then MOVE any pre-statements it pushed out of
	/// prePendingStatements into `_capturedOut`. The caller gates those behind the
	/// operand's execution condition instead of letting them run unconditionally.
	/// `_capturedOut` is left empty when the operand pushed nothing (the common,
	/// effect-free case — the caller can then skip all gating).
	template <class BuildFn>
	auto buildScopedOperand(
		BuildFn&& _build,
		std::vector<std::shared_ptr<awst::Statement>>& _capturedOut)
		-> decltype(_build())
	{
		ConditionalRegion region(*this);
		auto before = prePendingStatements.size();
		auto value = _build();
		if (prePendingStatements.size() > before)
		{
			_capturedOut.assign(
				std::make_move_iterator(prePendingStatements.begin() + before),
				std::make_move_iterator(prePendingStatements.end()));
			prePendingStatements.erase(
				prePendingStatements.begin() + before, prePendingStatements.end());
		}
		return value;
	}

	/// A block that runs `_preStmts` (the operand's captured pre-statements) then
	/// assigns `_value` to `_resultTarget` — the shape both the ternary branches
	/// and the short-circuit RHS wrap their gated operand in.
	static std::shared_ptr<awst::Block> makeScopedResultBlock(
		std::vector<std::shared_ptr<awst::Statement>> _preStmts,
		std::shared_ptr<awst::Expression> _resultTarget,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc)
	{
		auto block = awst::makeBlock(_loc);
		for (auto& s: _preStmts)
			block->body.push_back(std::move(s));
		block->body.push_back(
			awst::makeAssignmentStatement(std::move(_resultTarget), std::move(_value), _loc));
		return block;
	}

	// ── Per-translation scope state (owned) ──

	/// Innermost active scope; null before the first pushScopeRaii.
	sol_ast::Context* currentScope = nullptr;

	/// RAII scope guard. Use: `auto guard = ctx.pushScopeRaii(&someContext);`.
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

	/// RHS stash for `arr.push() = value`: SolAssignment writes here before building
	/// the LHS; SolArrayMethod::push() consumes it as the element (not the default).
	std::shared_ptr<awst::Expression> pendingArrayPushValue;

	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	/// Fallback binary-op when sol-eb builders don't handle the operation.
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Token, std::shared_ptr<awst::Expression>,
		std::shared_ptr<awst::Expression>, awst::WType const*,
		awst::SourceLocation const&)> buildBinaryOp;

	/// Returns nullptr if no builder is registered for the Solidity type.
	std::function<std::unique_ptr<InstanceBuilder>(
		solidity::frontend::Type const*, std::shared_ptr<awst::Expression>)> builderForInstance;

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
// Alias into puyasol::builder so callers outside eb/ can use builder::ContractContext.
using ContractContext = eb::ContractContext;
}
