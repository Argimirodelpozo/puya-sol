/// @file SolExpressionStatement.cpp
/// ExpressionStatement, RevertStatement, ReturnStatement.

#include "builder/ast/stmts/SolExpressionStatement.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/eb/MappingPrefix.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/contract/AWSTBuilder.h" // containsMappingType
#include "builder/ast/calls/RevertBlob.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/context/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/ConversionPlan.h"
#include "awst/TupleValue.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

// ── ExpressionStatement ──

SolExpressionStatement::SolExpressionStatement(
	BlockContext& _blk, ExpressionStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolExpressionStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;

	// Type expressions as statements (e.g. `s[7][];`) resolve to a type
	// value with no runtime representation. We still need to walk the
	// expression tree to pick up side effects (e.g. `((flag = true) ? M : M).D;`
	// needs the assignment to happen) but we must not emit the final value
	// expression because our type mapper can't model it.
	bool isTypeType = dynamic_cast<solidity::frontend::TypeType const*>(
		m_node.expression().annotation().type) != nullptr;

	auto expr = m_blk.builderCtx().buildExpr(m_node.expression());

	for (auto& p: m_blk.builderCtx().takePreEffects())
		result.push_back(std::move(p));

	// If buildExpr couldn't produce a value expression, or the expression
	// is a type-valued expression, skip emitting the final statement to
	// avoid a null dereference or invalid AWST.
	if (!expr || isTypeType)
	{
		for (auto& p: m_blk.builderCtx().takePostEffects())
			result.push_back(std::move(p));
		return result;
	}

	auto stmt = awst::makeExpressionStatement(std::move(expr), m_loc);
	result.push_back(stmt);

	for (auto& p: m_blk.builderCtx().takePostEffects())
		result.push_back(std::move(p));

	return result;
}

// ── RevertStatement ──

SolRevertStatement::SolRevertStatement(
	BlockContext& _blk, RevertStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolRevertStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;

	RevertPayload payload(m_blk.builderCtx(), m_node.errorCall(), m_loc);
	for (auto& stmt: m_blk.builderCtx().takePreEffects())
		result.push_back(std::move(stmt));
	result.push_back(makeRevertLogStmt(std::move(payload.blob), m_loc));
	auto const& errorName = payload.message;
	auto failNode = awst::makeAssert(awst::makeFalse(m_loc), m_loc, errorName);
	// The log (when present) carries the user-visible revert contract; let
	// puya's optimizer strip the fail when provably unreachable.
	failNode->isExplicit = false;
	result.push_back(awst::makeExpressionStatement(std::move(failNode), m_loc));
	return result;
}

// ── ReturnStatement ──

SolReturnStatement::SolReturnStatement(
	BlockContext& _blk, Return const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

namespace
{

/// Storage-reference return components carry logical slots when the wire plan requires them.
bool trySlotStorageReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::shared_ptr<awst::ReturnStatement>& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	auto const& rps = node.annotation().functionReturnParameters->parameters();
	auto const* returnTuple = dynamic_cast<awst::WTuple const*>(blk.fn.returnType);
	auto slotReturn = [&](size_t i) {
		return rps[i]->referenceLocation() == VariableDeclaration::Location::Storage
			&& (returnTuple ? returnTuple->types().at(i) : blk.fn.returnType)
				== awst::WType::biguintType();
	};
	if (rps.size() == 1
		&& slotReturn(0))
	{
		EvmSlotLowering low(blk.builderCtx(), blk.scope, loc);
		auto addr = low.resolve(*node.expression());
		if (!addr)
			return true;   // error already logged
		stmt->value = addr->slot;
		blk.builderCtx().appendEffectsTo(result);
		result.push_back(std::move(stmt));
		return true;
	}
	// MULTI-value return with storage component(s):
	// `return (1, 2, data)` where the 3rd is `T storage`. The
	// generic build would MATERIALISE the aggregate (or
	// reject it); the declared slot-handle convention wants the
	// biguint slot in that position. Build component-wise.
	bool anyStorageRet = false;
	for (size_t i = 0; i < rps.size(); ++i)
		anyStorageRet |= slotReturn(i);
	if (rps.size() > 1 && anyStorageRet)
	{
		auto& ctx = blk.builderCtx();
		auto build = [&](auto&& self, Expression const* source) -> std::shared_ptr<awst::Expression> {
			auto const* srcTup = dynamic_cast<solidity::frontend::TupleExpression const*>(source);
			while (srcTup && !srcTup->isInlineArray() && srcTup->components().size() == 1)
			{
				source = srcTup->components()[0].get();
				srcTup = dynamic_cast<solidity::frontend::TupleExpression const*>(source);
			}
			// Select references, not copies of their values. Each branch keeps its
			// own effects and applies the declared return-component conversions.
			if (auto const* conditional = dynamic_cast<Conditional const*>(source))
			{
				auto condition = ctx.pinIfWriteBacks(ctx.lower(conditional->condition(), false), loc);
				condition = ctx.emitSequencedOperand({}, std::move(condition), true, loc);
				auto whenTrue = ctx.lowerOperand([&] { return self(self, &conditional->trueExpression()); });
				auto whenFalse = ctx.lowerOperand([&] { return self(self, &conditional->falseExpression()); });
				if (!whenTrue.value || !whenFalse.value) return nullptr;
				auto const* type = whenTrue.value->wtype;
				return ctx.emitConditional(std::move(condition), std::move(whenTrue),
					std::move(whenFalse), type, loc);
			}
			std::vector<std::shared_ptr<awst::Expression>> opaqueItems;
			auto const* sourceTypes = dynamic_cast<TupleType const*>(source->annotation().type);
			if (!srcTup)
			{
				auto opaque = ctx.pinIfWriteBacks(ctx.lower(*source, false), loc);
				opaque = ctx.emitSequencedOperand({}, std::move(opaque), true, loc);
				opaqueItems = awst::tupleItems(std::move(opaque), loc);
			}
			EvmSlotLowering low(ctx, blk.scope, loc);
			auto tup = awst::makeTupleExpression(nullptr, loc);
			std::vector<awst::WType const*> wts;
			for (size_t ri = 0; ri < rps.size(); ++ri)
			{
				auto const* compExpr = srcTup ? srcTup->components().at(ri).get() : nullptr;
				auto const* sourceType = compExpr ? compExpr->annotation().type : sourceTypes->components().at(ri);
				std::shared_ptr<awst::Expression> v;
				if (!opaqueItems.empty())
					v = std::move(opaqueItems.at(ri));
				else if (slotReturn(ri))
				{
					auto addr = low.resolve(*compExpr);
					if (!addr) return nullptr;   // error already logged
					v = addr->slot;
				}
				else
					v = ctx.buildExpr(*compExpr);
				if (!slotReturn(ri))
				{
					auto const* target = blk.typeMapper().map(rps[ri]->type());
					v = EvmSlotLowering::materializeRefValue(ctx, blk.scope,
						std::move(v), sourceType, target, loc);
					v = builder::ConversionPlan{sourceType, rps[ri]->type(), target,
						builder::ConversionPlan::Context::Return}.emit(std::move(v), loc, &ctx.preEffects());
				}
				if (!v) return nullptr;
				if (slotReturn(ri) && v->wtype != awst::WType::biguintType())
				{
					Logger::instance().error("storage-reference return lost its logical slot", loc);
					return nullptr;
				}
				wts.push_back(v->wtype);
				tup->items.push_back(std::move(v));
			}
			tup->wtype = blk.typeMapper().createType<awst::WTuple>(std::move(wts), std::nullopt);
			return tup;
		};
		stmt->value = build(build, node.expression());
		if (!stmt->value) return true;
		blk.builderCtx().appendEffectsTo(result);
		result.push_back(std::move(stmt));
		return true;
	}
	return false;
}

/// Box-keyed mapping-of-struct storage-ref RETURN, e.g.
bool tryBoxKeyedRefReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::shared_ptr<awst::ReturnStatement>& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	auto const& rps = node.annotation().functionReturnParameters->parameters();
	bool const storageRefMapReturn = rps.size() == 1
		&& rps[0]->referenceLocation() == VariableDeclaration::Location::Storage
		&& blk.typeMapper().isBoxKeyedStorageRef(rps[0]->type());
	if (storageRefMapReturn && containsMappingType(node.expression()->annotation().type))
	{
		stmt->value = storageReferenceKey(blk.builderCtx(), blk.scope, *node.expression(), loc);
		blk.builderCtx().appendEffectsTo(result);
		result.push_back(std::move(stmt));
		return true;
	}
	if (!storageRefMapReturn
		|| !dynamic_cast<solidity::frontend::IndexAccess const*>(node.expression()))
		return false;

	auto built = blk.builderCtx().buildExpr(*node.expression());
	built = awst::unwrapStateGet(std::move(built));
	if (auto* box = dynamic_cast<awst::BoxValueExpression*>(built.get()))
		stmt->value = awst::makeReinterpretCast(
			box->key, awst::WType::bytesType(), loc);
	else
		stmt->value = std::move(built);
	blk.builderCtx().appendEffectsTo(result); // pending before the return
	result.push_back(std::move(stmt));
	return true;
}

/// Single declared return: coerce the built value to the declared type.
/// Scratch model, offset-protocol function (`fn.returnType` is uint64 for a
/// memory aggregate): `return <reference>` hands back the object's offset,
/// `return <fresh value>` spills the value first. Solc returns the pointer.
bool tryScratchReferenceReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<ASTPointer<VariableDeclaration>> const& retParams,
	awst::ReturnStatement& stmt)
{
	auto& ctx = blk.builderCtx();
	if (!ctx.typeMapper.profile().scratchMemoryModel || retParams.size() != 1
		|| blk.fn.returnType != awst::WType::uint64Type())
		return false;
	auto const& target = *retParams[0];
	if (target.referenceLocation() != VariableDeclaration::Location::Memory)
		return false;
	auto const* wtype = blk.typeMapper().map(target.type());
	if (!builder::isAggregateCarrier(wtype))
		return false;
	if (auto reference = SolIndexAccess::resolveBlobReference(
			ctx, blk.scope, *node.expression(), loc))
	{
		stmt.value = ctx.emitSequencedOperand(
			std::move(reference->effects), std::move(reference->value), true, loc);
		return true;
	}
	auto value = ctx.pinIfWriteBacks(ctx.lower(*node.expression(), false), loc);
	if (!value)
		return true;
	value = builder::TypeCoercion::coerceForAssignment(std::move(value), wtype, loc);
	auto id = awst::NameGen::next("SolReturnStatement.freshReference");
	std::string name = "__ret_ref_" + std::to_string(id);
	if (!builder::spillEvmMemoryValue(blk.typeMapper(), target.type(), wtype,
			std::move(value), name, id, loc, ctx.preEffects()))
		throw std::runtime_error("Cannot spill returned memory value into scratch memory");
	stmt.value = awst::makeVarExpression(name, awst::WType::uint64Type(), loc);
	return true;
}

void convertSingleReturnValue(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<ASTPointer<VariableDeclaration>> const& retParams,
	awst::ReturnStatement& stmt)
{
	auto const* targetSolType = retParams[0]->type();
	auto const* targetWType = blk.typeMapper().map(targetSolType);
	if (!targetSolType->dataStoredIn(DataLocation::Storage))
		stmt.value = StorageMapper::makePartialBoxReadWithDefault(
			blk.typeMapper(), std::move(stmt.value), blk.builderCtx().preEffects(), loc);
	// Slot mode: `return <storage ref>` from a MEMORY-typed return
	// materializes the aggregate (the storage-declared return case
	// exited earlier with the raw slot).
	stmt.value = EvmSlotLowering::materializeRefValue(
		blk.builderCtx(), blk.scope, std::move(stmt.value),
		node.expression()->annotation().type, targetWType, loc);
	stmt.value = builder::ConversionPlan{
		node.expression()->annotation().type,
		targetSolType,
		targetWType,
		builder::ConversionPlan::Context::Return}.emit(
			std::move(stmt.value), loc);
}

/// Multi-value returns use the solc component types even for opaque calls and ternaries.
void convertTupleReturnValue(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<ASTPointer<VariableDeclaration>> const& retParams,
	awst::ReturnStatement& stmt)
{
	auto const* source = dynamic_cast<solidity::frontend::TupleType const*>(
		node.expression()->annotation().type);
	assert(source && source->components().size() == retParams.size());
	auto& ctx = blk.builderCtx();
	auto items = awst::tupleItems(std::move(stmt.value), loc, &ctx.preEffects());
	assert(items.size() == retParams.size());
	auto result = awst::makeTupleExpression(nullptr, loc);
	std::vector<awst::WType const*> types;
	for (size_t i = 0; i < retParams.size(); ++i)
	{
		auto const* target = retParams[i]->type();
		auto const* representation = blk.typeMapper().map(target);
		auto value = EvmSlotLowering::materializeRefValue(
			ctx, blk.scope, std::move(items[i]), source->components()[i], representation, loc);
		value = builder::ConversionPlan{source->components()[i], target, representation,
			builder::ConversionPlan::Context::Return}.emit(std::move(value), loc, &ctx.preEffects());
		types.push_back(value->wtype);
		result->items.push_back(std::move(value));
	}
	result->wtype = blk.typeMapper().createType<awst::WTuple>(std::move(types), std::nullopt);
	stmt.value = std::move(result);
}

/// Enum range validation on return: EVM panics (0x21) on invalid enum return values.
void maybeAppendEnumReturnAssert(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc, awst::ReturnStatement& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!stmt.value)
		return;
	auto const& retAnnotation = node.annotation();
	if (!retAnnotation.functionReturnParameters)
		return;
	auto const& retParams = retAnnotation.functionReturnParameters->parameters();
	if (retParams.size() != 1)
		return;
	auto const* enumType = dynamic_cast<EnumType const*>(retParams[0]->type());
	if (!enumType)
		return;

	unsigned numMembers = enumType->numberOfMembers();
	// The value feeds both the range-assert and the return —
	// wrap so `return f()` with a side-effecting enum f()
	// evaluates once (verified: f() ran twice).
	stmt.value = awst::makeEvalOnce(std::move(stmt.value), loc);
	auto val = builder::TypeCoercion::implicitNumericCast(
		stmt.value, awst::WType::uint64Type(), loc);

	auto assertStmt = awst::makeExpressionStatement(
		awst::makeEnumRangeAssert(val, numMembers, loc), loc);
	result.push_back(std::move(assertStmt));
}

} // anonymous namespace

std::vector<std::shared_ptr<awst::Statement>> SolReturnStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;

	auto stmt = awst::makeReturnStatement(nullptr, m_loc);

	// Solc rejects bare returns in value-returning functions. Modifier exits
	// stay bare here; ModifierChainBuilder attaches their threaded results.
	if (m_node.expression())
	{
		auto const& retParams = m_node.annotation().functionReturnParameters->parameters();
		if (retParams.empty())
		{
			// `return voidCall();`: complete the call and its write-backs before exit.
			m_blk.builderCtx().evaluateForEffects(*m_node.expression(), m_loc);
			m_blk.builderCtx().appendEffectsTo(result);
			result.push_back(std::move(stmt));
			return result;
		}
		if (trySlotStorageReturn(m_blk, m_node, m_loc, stmt, result))
			return result;
		if (tryBoxKeyedRefReturn(m_blk, m_node, m_loc, stmt, result))
			return result;
		if (tryScratchReferenceReturn(m_blk, m_node, m_loc, retParams, *stmt))
		{
			m_blk.builderCtx().appendEffectsTo(result);
			result.push_back(stmt);
			return result;
		}

		stmt->value = m_blk.builderCtx().buildExpr(*m_node.expression());
		if (!stmt->value)
			return result;   // build errored (already logged) — don't deref

		if (retParams.size() == 1)
			convertSingleReturnValue(m_blk, m_node, m_loc, retParams, *stmt);
		else
			convertTupleReturnValue(m_blk, m_node, m_loc, retParams, *stmt);
	}

	m_blk.builderCtx().appendEffectsTo(result);

	maybeAppendEnumReturnAssert(m_blk, m_node, m_loc, *stmt, result);

	// D2 build-time ABI return encoding: wrap the (already value-coerced) return
	// value in its ABI wire type right here. Scalar + tuple
	// (literal / ternary / opaque-spill).
	// Modifier chains instead normalize native returns and encode only their
	// outer wrapper. Implicit named returns are constructed by FunctionBuilder.
	if (m_blk.fn.encodeReturnsAtBuildTime && stmt->value)
	{
		auto valLoc = stmt->value->sourceLocation;
		std::vector<std::shared_ptr<awst::Statement>> prepend;
		stmt->value = builder::TypeCoercion::encodeReturnValue(
			m_blk.typeMapper(), std::move(stmt->value), m_blk.fn.returnWirePlan,
			valLoc, prepend,
			m_blk.fn.returnAsmWrap);
		// Opaque-tuple spill assignment(s) go before the return.
		for (auto& s: prepend)
			result.push_back(std::move(s));
	}

	result.push_back(stmt);
	return result;
}

} // namespace puyasol::builder::sol_ast
