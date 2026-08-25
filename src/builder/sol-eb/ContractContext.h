#pragma once

#include "awst/Node.h"
#include "builder/FunctionSymbolTable.h"

#include "builder/sol-types/SolcFwd.h"
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

/// Central context for expression/statement builders: owns structurally scoped
/// effect frames, references compiler services (TypeMapper, StorageMapper), and
/// holds the builder registry. Expression and type-operation dispatch are
/// ordinary methods so their availability and lifetime follow the context itself.
class ContractContext
{
public:
	struct OperandDeltas
	{
		std::vector<std::shared_ptr<awst::Statement>> pre, post;
		bool empty() const { return pre.empty() && post.empty(); }
	};

	struct LoweredExpression
	{
		std::shared_ptr<awst::Expression> value;
		OperandDeltas effects;
		solidity::frontend::Type const* solType = nullptr;
	};

	ContractContext(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		std::unordered_set<std::string> const& _overloadedNames,
		FunctionSymbolTable const& _functionSymbols,
		FunctionPointerRegistry& _functionPointers
	);

	~ContractContext();

	ContractContext(ContractContext const&) = delete;
	ContractContext& operator=(ContractContext const&) = delete;
	ContractContext(ContractContext&&) = delete;
	ContractContext& operator=(ContractContext&&) = delete;

	/// Lower one Solidity expression to a value plus structurally owned effects
	/// and its source solc type. This is the primary expression API.
	LoweredExpression build(
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

	/// Returns nullptr if no builder is registered for the Solidity type.
	std::unique_ptr<InstanceBuilder> builderForInstance(
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr);

	/// Consume effects that execute after the translated expression value.
	std::vector<std::shared_ptr<awst::Statement>> takePostEffects();

	/// Consume effects that must execute before the translated expression value.
	std::vector<std::shared_ptr<awst::Statement>> takePreEffects();

	/// Drain pre- then post-effects into `_out` in execution order.
	void appendEffectsTo(std::vector<std::shared_ptr<awst::Statement>>& _out);

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
	/// Deployable contracts in this compilation unit. Freestanding library/free
	/// subroutines use their union when translating runtime ARC-4 selectors to
	/// Solidity selectors under --evm-selectors.
	std::vector<solidity::frontend::ContractDefinition const*> selectorContracts;

	// ── Function resolution tables (external, by reference) ──
	std::unordered_set<std::string> const& overloadedNames;
	/// Canonical solc declaration ID → opaque AWST symbol table.
	FunctionSymbolTable const& functionSymbols;
	FunctionPointerRegistry& functionPointers;
	/// funcDef.id() → synthesized method name; CallResolver returns InstanceMethodTarget
	/// instead of SubroutineID when the funcDef appears here.
	std::unordered_map<int64_t, std::string> internalizedFunctionNames;

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

	template <typename Value>
	struct LoweredValue
	{
		Value value;
		OperandDeltas effects;
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
		OperandDeltas effects;
		m_effectFrames.push_back(&effects);
		try
		{
			result.value = _build();
		}
		catch (...)
		{
			m_effectFrames.pop_back();
			throw;
		}
		m_effectFrames.pop_back();
		result.effects = std::move(effects);
		return result;
	}

	LoweredExpression lower(
		solidity::frontend::Expression const& _expr,
		bool _conditional = true)
	{
		return build(_expr, _conditional);
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

	/// Lexically scoped translation parameter for `arr.push() = value`.
	/// SolAssignment installs it while lowering the LHS call and SolArrayMethod
	/// consumes it. The RAII scope prevents a failed/throwing LHS from leaking the
	/// value into an unrelated later push.
	class ArrayPushAssignmentScope
	{
	public:
		ArrayPushAssignmentScope(
			ContractContext& _ctx,
			std::shared_ptr<awst::Expression> _value)
			: m_ctx(_ctx)
		{
			m_ctx.m_arrayPushAssignmentValues.push_back(std::move(_value));
		}
		~ArrayPushAssignmentScope()
		{
			m_ctx.m_arrayPushAssignmentValues.pop_back();
		}
		ArrayPushAssignmentScope(ArrayPushAssignmentScope const&) = delete;
		ArrayPushAssignmentScope& operator=(ArrayPushAssignmentScope const&) = delete;
	private:
		ContractContext& m_ctx;
	};

	[[nodiscard]] ArrayPushAssignmentScope pushArrayAssignmentValue(
		std::shared_ptr<awst::Expression> _value)
	{
		return ArrayPushAssignmentScope(*this, std::move(_value));
	}

	bool hasArrayAssignmentValue() const
	{
		return !m_arrayPushAssignmentValues.empty()
			&& static_cast<bool>(m_arrayPushAssignmentValues.back());
	}

	std::shared_ptr<awst::Expression> takeArrayAssignmentValue()
	{
		if (m_arrayPushAssignmentValues.empty())
			return nullptr;
		return std::move(m_arrayPushAssignmentValues.back());
	}

	awst::SourceLocation makeLoc(int _start, int _end) const;

private:
	std::shared_ptr<awst::Expression> buildValue(
		solidity::frontend::Expression const& _expr);

	OperandDeltas& activeEffects() const
	{
		return m_effectFrames.empty() ? m_rootEffects : *m_effectFrames.back();
	}

	mutable OperandDeltas m_rootEffects;
	std::vector<OperandDeltas*> m_effectFrames;
	std::vector<std::shared_ptr<awst::Expression>> m_arrayPushAssignmentValues;

};

} // namespace puyasol::builder::eb

namespace puyasol::builder
{
// Alias into puyasol::builder so callers outside eb/ can use builder::ContractContext.
using ContractContext = eb::ContractContext;
}
