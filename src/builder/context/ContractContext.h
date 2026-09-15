#pragma once

#include "awst/Node.h"

#include "builder/solc/SolcFwd.h"
#include <liblangutil/Token.h>
#include <libsolutil/Common.h>

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

namespace solidity::langutil { struct SourceLocation; }

namespace puyasol::builder
{
class TypeMapper;
class StorageMapper;
class StorageBackend;
class StorageLayout;
class TransientStorage;
namespace sol_ast {
struct Context;
}
}

namespace puyasol::builder::eb
{

class InstanceBuilder;
struct FunctionPointerRegistry;

/// Central context for expression/statement builders: owns structurally scoped
/// effect frames and references compiler services (TypeMapper, StorageMapper).
/// Expression and type-operation dispatch are
/// ordinary methods so their availability and lifetime follow the context itself.
class ContractContext
{
public:
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

	ContractContext(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		std::unordered_set<std::string> const& _overloadedNames,
		FunctionPointerRegistry& _functionPointers
	);

	~ContractContext();

	ContractContext(ContractContext const&) = delete;
	ContractContext& operator=(ContractContext const&) = delete;
	ContractContext(ContractContext&&) = delete;
	ContractContext& operator=(ContractContext&&) = delete;

	/// Lower one Solidity expression to a value plus structurally owned effects.
	LoweredExpression lower(
		solidity::frontend::Expression const& _expr,
		bool _conditional = true);

	/// Compatibility composition point for expression-builder implementations:
	/// lower the child structurally, then attach its effects to the active parent
	/// frame at the exact evaluation position.
	std::shared_ptr<awst::Expression> buildExpr(
		solidity::frontend::Expression const& _expr);

	/// Evaluate a Solidity expression whose value is intentionally discarded.
	/// This preserves the complete pre/value/post sequence at the current
	/// evaluation position; callers must not merely call buildExpr() and drop
	/// the returned AWST node, because the value node itself may carry effects.
	void evaluateForEffects(
		solidity::frontend::Expression const& _expr,
		awst::SourceLocation const& _loc);

	/// Fallback binary operation when the type-specific builder does not handle it.
	std::shared_ptr<awst::Expression> buildBinaryOp(
		// langutil::Token IS frontend::Token -- the latter was only visible
		// through a using-directive inside Types.h. Naming it directly keeps
		// the cheap <liblangutil/Token.h> above sufficient.
		solidity::langutil::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc);

	/// Returns nullptr for unsupported solc type categories.
	std::unique_ptr<InstanceBuilder> builderForInstance(
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr);

	/// Consume effects that execute after the translated expression value.
	std::vector<std::shared_ptr<awst::Statement>> takePostEffects();

	/// Consume effects that must execute before the translated expression value.
	std::vector<std::shared_ptr<awst::Statement>> takePreEffects();

	/// Drain pre- then post-effects into `_out` in execution order.
	void appendEffectsTo(std::vector<std::shared_ptr<awst::Statement>>& _out);

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
	/// Deployable contracts in this compilation unit. Freestanding library/free
	/// subroutines use their union when translating runtime ARC-4 selectors to
	/// Solidity selectors under --evm-selectors.
	std::vector<solidity::frontend::ContractDefinition const*> selectorContracts;

	// ── Function resolution tables (external, by reference) ──
	std::unordered_set<std::string> const& overloadedNames;
	FunctionPointerRegistry& functionPointers;
	/// funcDef.id() → synthesized method name; CallResolver returns InstanceMethodTarget
	/// instead of SubroutineID when the funcDef appears here.
	std::unordered_map<int64_t, std::string> internalizedFunctionNames;
	/// Concrete super/explicit-base implementations requested while lowering.
	/// IDs deduplicate emission, including recursive and transitive requests.
	std::unordered_set<int64_t> baseImplementationIds;
	std::vector<solidity::frontend::FunctionDefinition const*> pendingBaseImplementations;

	// ── Structurally scoped expression effects ──
	using EffectStatements = std::vector<std::shared_ptr<awst::Statement>>;

	/// Direct access to the innermost structural frame. Helpers that need to
	/// receive a statement sink use these references; no context-wide adapter
	/// or implicit vector conversion remains.
	EffectStatements& preEffects() { return activeEffects().pre; }
	EffectStatements& postEffects() { return activeEffects().post; }

	/// Append an expression as a post-effect (after the current value is consumed).
	void queuePostExpression(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		postEffects().push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Append a statement directly as a post-effect.
	void queuePostEffect(std::shared_ptr<awst::Statement> stmt)
	{
		postEffects().push_back(std::move(stmt));
	}

	/// Append an expression as a pre-effect (before the current value is consumed).
	void queuePreExpression(std::shared_ptr<awst::Expression> expr, awst::SourceLocation loc)
	{
		preEffects().push_back(awst::makeExpressionStatement(std::move(expr), std::move(loc)));
	}

	/// Append a statement directly as a pre-effect.
	void queuePreEffect(std::shared_ptr<awst::Statement> stmt)
	{
		preEffects().push_back(std::move(stmt));
	}

	// ── OperandPlan: scoped effect sequencing (fable-review item 7) ──
	// Pre-statements (overflow/zero asserts, `**` loops, box materializations,
	// inner-txn side effects) are accumulated in the active structural frame.
	// Flushing them unconditionally before the current statement would be wrong
	// for an operand that only executes CONDITIONALLY — a ternary branch, a
	// short-circuit RHS — where its pre-statements must be gated behind the same
	// condition. Every such site used to hand-roll a snapshot/extract/erase of the
	// list (the C1 bug class: short-circuit RHS hoist 5a1f5810ad, ternary operand
	// SE cd9d91ccfa). These two primitives own that invariant in one place.

	/// Depth of CONDITIONALLY-EXECUTED translation regions (if/else branches,
	/// loop bodies, ternary/short-circuit arms). Compile-time-only state
	/// mutations — storage-pointer rebinds recorded in the scope bindings — are
	/// UNSOUND inside one (the rebind would apply unconditionally to all
	/// later uses); producers of such state must fail loud when depth > 0.
	int conditionalDepth = 0;

	/// RAII marker for a conditionally-executed translation region.
	[[nodiscard]] auto conditionalRegion()
	{
		return solidity::ScopedSaveAndRestore(conditionalDepth, conditionalDepth + 1);
	}

	/// Solidity's two codegen pipelines disagree on intra-expression
	/// evaluation order (unspecified by the language): legacy evaluates a
	/// binop's RIGHT operand first, via-IR LEFT-to-right. false (default) =
	/// legacy order; true (--via-yul-behavior) keeps build order untouched.
	bool viaIRSequencing = false;

	/// Scratch memory model: the next internal call lowered while this is set
	/// yields a memory-aggregate result as its uint64 offset rather than a
	/// materialized value. Set only through MemoryReferenceScope.
	bool memoryReferenceWanted = false;

	class MemoryReferenceScope
	{
	public:
		explicit MemoryReferenceScope(ContractContext& _ctx)
			: m_ctx(_ctx), m_previous(_ctx.memoryReferenceWanted) { m_ctx.memoryReferenceWanted = true; }
		~MemoryReferenceScope() { m_ctx.memoryReferenceWanted = m_previous; }
		MemoryReferenceScope(MemoryReferenceScope const&) = delete;
		MemoryReferenceScope& operator=(MemoryReferenceScope const&) = delete;
	private:
		ContractContext& m_ctx;
		bool m_previous;
	};

	/// Put a captured operand's deltas back exactly where they came from
	/// (pre → pre-effects, post → post-effects) — the no-reorder path.
	void restoreOperandDeltas(OperandDeltas&& _d)
	{
		for (auto& s: _d.pre)
			preEffects().push_back(std::move(s));
		for (auto& s: _d.post)
			postEffects().push_back(std::move(s));
	}

	/// Build an operand in its own effect frame, then return its value together
	/// with the pre/post statements it produced. Pass
	/// `_conditional = true` when the operand executes conditionally (ternary
	/// branch, short-circuit RHS) — it marks a conditional region and the caller
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
		solidity::ScopedSaveAndRestore region(conditionalDepth, conditionalDepth + int(_conditional));
		solidity::ScopedSaveAndRestore frame(m_effectFrame, &result.effects);
		result.value = _build();
		return result;
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

	/// Gate each branch's complete pre/value/post sequence behind the condition.
	/// Pure branches remain expressions; effectful branches share a result local.
	std::shared_ptr<awst::Expression> emitConditional(
		std::shared_ptr<awst::Expression> _condition,
		LoweredValue<std::shared_ptr<awst::Expression>> _true,
		LoweredValue<std::shared_ptr<awst::Expression>> _false,
		awst::WType const* _type,
		awst::SourceLocation const& _loc);

	/// Unconditionally-evaluated operand (ternary condition, short-circuit
	/// LEFT): re-emit its effects, pinning the value only when it carried
	/// write-backs so later reads observe them while the pinned value keeps
	/// its pre-write-back reads. The shape shared by SolConditional,
	/// trySolShortCircuit, and the slot-mode conditional lowering.
	std::shared_ptr<awst::Expression> pinIfWriteBacks(
		LoweredExpression&& _low, awst::SourceLocation const& _loc)
	{
		bool const hadPost = !_low.effects.post.empty();
		return emitSequencedOperand(
			std::move(_low.effects), std::move(_low.value), hadPost, _loc);
	}

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

	/// Active scope for expression lowering; requires a pushed or root scope.
	sol_ast::Context& scope() const;

	/// RAII scope guard. Use: `auto guard = ctx.pushScopeRaii(&block.scope);`.
	[[nodiscard]] auto pushScopeRaii(sol_ast::Context* _scope)
	{
		return solidity::ScopedSaveAndRestore(currentScope, std::move(_scope));
	}

	/// Lexically scoped translation parameter for `arr.push() = value`.
	/// SolAssignment installs it while lowering the LHS call and SolArrayMethod
	/// consumes it. The RAII scope prevents a failed/throwing LHS from leaking the
	/// value into an unrelated later push.
	[[nodiscard]] auto pushArrayAssignmentValue(
		std::shared_ptr<awst::Expression> _value)
	{
		return solidity::ScopedSaveAndRestore(m_arrayPushAssignmentValue, std::move(_value));
	}

	bool hasArrayAssignmentValue() const
	{
		return static_cast<bool>(m_arrayPushAssignmentValue);
	}

	std::shared_ptr<awst::Expression> takeArrayAssignmentValue()
	{
		return std::exchange(m_arrayPushAssignmentValue, {});
	}

	awst::SourceLocation makeLoc(int _start, int _end) const;
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _location) const;

private:
	std::shared_ptr<awst::Expression> buildValue(
		solidity::frontend::Expression const& _expr);

	OperandDeltas& activeEffects() const
	{
		return m_effectFrame ? *m_effectFrame : m_rootEffects;
	}

	mutable OperandDeltas m_rootEffects;
	OperandDeltas* m_effectFrame = nullptr;
	std::shared_ptr<awst::Expression> m_arrayPushAssignmentValue;

};

} // namespace puyasol::builder::eb

namespace puyasol::builder
{
// Alias into puyasol::builder so callers outside eb/ can use builder::ContractContext.
using ContractContext = eb::ContractContext;
}
