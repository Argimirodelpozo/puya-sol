#pragma once

#include "awst/Node.h"

#include <libsolidity/ast/Types.h>
#include <liblangutil/Token.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
class StorageLayout;
class TransientStorage;
namespace sol_ast {
class Context;
}
}

namespace puyasol::builder::eb
{

class InstanceBuilder;
class BuilderRegistry;
struct FunctionPointerRegistry;

/// Central context for expression/statement builders: owns pending-statement buffers,
/// references compiler services (TypeMapper, StorageMapper), and holds the builder
/// registry. Expression and type-operation dispatch are ordinary methods so
/// their availability and lifetime follow the context itself.
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
		std::unordered_map<int64_t, std::string> const& _freeFunctionById,
		FunctionPointerRegistry& _functionPointers
	);

	~ContractContext();

	ContractContext(ContractContext const&) = delete;
	ContractContext& operator=(ContractContext const&) = delete;
	ContractContext(ContractContext&&) = delete;
	ContractContext& operator=(ContractContext&&) = delete;

	/// Build an AWST expression from a Solidity expression. Primary entry point.
	std::shared_ptr<awst::Expression> build(solidity::frontend::Expression const& _expr);
	std::shared_ptr<awst::Expression> buildExpr(solidity::frontend::Expression const& _expr)
	{
		return build(_expr);
	}

	/// Fallback binary operation when the type-specific builder does not handle it.
	std::shared_ptr<awst::Expression> buildBinaryOp(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc);

	/// Returns nullptr if no builder is registered for the Solidity type.
	std::unique_ptr<InstanceBuilder> builderForInstance(
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr);

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
	/// Canonical layout for the contract currently being translated. Both the
	/// EVM slot lowerer and inline-assembly routing consume the session-owned
	/// instance; null for free/library subroutines without a concrete host.
	StorageLayout const* storageLayout = nullptr;
	std::string const& sourceFile;
	std::string const& contractName;
	/// Current contract (nullptr during free-function translation).
	solidity::frontend::ContractDefinition const* currentContract = nullptr;

	// ── Function resolution tables (external, by reference) ──
	std::unordered_map<std::string, std::string> const& libraryFunctionIds;
	std::unordered_set<std::string> const& overloadedNames;
	std::unordered_map<int64_t, std::string> const& freeFunctionById;
	FunctionPointerRegistry& functionPointers;
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

	/// Solidity's two codegen pipelines disagree on intra-expression
	/// evaluation order (unspecified by the language): legacy evaluates a
	/// binop's RIGHT operand first, via-IR LEFT-to-right. false (default) =
	/// legacy order; true (--via-yul-behavior) keeps build order untouched.
	bool viaIRSequencing = false;

	/// The pre- and post-pending statements one operand's build queued
	/// (OperandPlan). `pre` must run before the operand's value is produced,
	/// `post` right after (write-backs) — i.e. before any LATER-evaluated
	/// sibling operand, not at the statement boundary.
	struct OperandDeltas
	{
		std::vector<std::shared_ptr<awst::Statement>> pre, post;
		bool empty() const { return pre.empty() && post.empty(); }
	};

	template <typename Value>
	struct LoweredValue
	{
		Value value;
		OperandDeltas effects;
	};

	using LoweredExpression = LoweredValue<std::shared_ptr<awst::Expression>>;

	/// Put a captured operand's deltas back exactly where they came from
	/// (pre → prePending, post → pending) — the no-reorder path.
	void restoreOperandDeltas(OperandDeltas&& _d)
	{
		for (auto& s: _d.pre)
			prePendingStatements.push_back(std::move(s));
		for (auto& s: _d.post)
			pendingStatements.push_back(std::move(s));
	}

	/// Build an operand via `_build`, then return its value together with the
	/// pre/post pending statements it produced. Pass
	/// `_conditional = true` when the operand executes conditionally (ternary
	/// branch, short-circuit RHS) — it marks a ConditionalRegion and the caller
	/// gates the effects behind the condition. `false` for pure re-ORDERING to
	/// legacy-solc evaluation order (binop right-before-left, assignment
	/// RHS-first, call args left-to-right), where effects still run
	/// unconditionally. `effects` stays empty in the common effect-free case.
	template <class BuildFn>
	auto lowerOperand(
		BuildFn&& _build,
		bool _conditional = true)
		-> LoweredValue<decltype(_build())>
	{
		LoweredValue<decltype(_build())> result;
		std::optional<ConditionalRegion> region;
		if (_conditional)
			region.emplace(*this);
		auto preBefore = prePendingStatements.size();
		auto postBefore = pendingStatements.size();
		result.value = _build();
		auto moveTail = [](auto& _buf, size_t _from, auto& _outVec) {
			if (_buf.size() <= _from)
				return;
			_outVec.insert(_outVec.end(),
				std::make_move_iterator(_buf.begin() + _from),
				std::make_move_iterator(_buf.end()));
			_buf.erase(_buf.begin() + _from, _buf.end());
		};
		moveTail(prePendingStatements, preBefore, result.effects.pre);
		moveTail(pendingStatements, postBefore, result.effects.post);
		return result;
	}

	LoweredExpression lower(
		solidity::frontend::Expression const& _expr,
		bool _conditional = true)
	{
		return lowerOperand([&] { return build(_expr); }, _conditional);
	}

	/// Re-emit a captured operand at its evaluation position: its pre-effects,
	/// then (when `_pin`) a temp pinning the value, then its post-effects
	/// HOISTED to run before any later-evaluated sibling. Constants skip the
	/// pin. With empty deltas and no pin this is a byte-identical no-op.
	/// Returns the (possibly pinned) value.
	std::shared_ptr<awst::Expression> emitSequencedOperand(
		OperandDeltas&& _d,
		std::shared_ptr<awst::Expression> _value,
		bool _pin,
		awst::SourceLocation const& _loc);

	/// A block that runs `_preStmts` (the operand's captured pre-statements),
	/// assigns `_value` to `_resultTarget`, then runs `_postStmts` (the
	/// operand's captured write-backs — gated WITH the operand, not left to
	/// leak to the statement boundary) — the shape both the ternary branches
	/// and the short-circuit RHS wrap their gated operand in.
	static std::shared_ptr<awst::Block> makeScopedResultBlock(
		std::vector<std::shared_ptr<awst::Statement>> _preStmts,
		std::shared_ptr<awst::Expression> _resultTarget,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>> _postStmts = {})
	{
		auto block = awst::makeBlock(_loc);
		for (auto& s: _preStmts)
			block->body.push_back(std::move(s));
		block->body.push_back(
			awst::makeAssignmentStatement(std::move(_resultTarget), std::move(_value), _loc));
		for (auto& s: _postStmts)
			block->body.push_back(std::move(s));
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

	awst::SourceLocation makeLoc(int _start, int _end) const;

};

} // namespace puyasol::builder::eb

namespace puyasol::builder
{
// Alias into puyasol::builder so callers outside eb/ can use builder::ContractContext.
using ContractContext = eb::ContractContext;
}
