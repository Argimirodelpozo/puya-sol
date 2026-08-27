/// @file SolExpressionStatement.cpp
/// ExpressionStatement, RevertStatement, ReturnStatement.

#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/SelectorSemantics.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/AWSTBuilder.h" // containsMappingType
#include "builder/sol-ast/calls/RevertBlob.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
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

	// Resolve via solc's ASTNode::referencedDeclaration helper so we get
	// the ErrorDefinition's name regardless of whether the source wrote
	// `revert MyError()` (Identifier), `revert Lib.MyError()`
	// (MemberAccess), or used an IdentifierPath.
	std::string errorName = "revert";
	if (auto const* errorDef = dynamic_cast<ErrorDefinition const*>(
			ASTNode::referencedDeclaration(m_node.errorCall().expression())))
	{
		errorName = errorDef->name();

		// Custom-error payload, logged before the failing `err` so clients read it
		// via simulate. Selector policy follows Error.selector; argument transport
		// remains ARC-4 until a full EVM ABI mode exists.
		auto const* errorType = errorDef->functionType(true);
		auto sig = errorType->externalSignature();
		std::shared_ptr<awst::Expression> blob =
			builder::SelectorSemantics::functionSelector(
				m_blk.builderCtx(), *errorType, sig, m_loc);
		auto const errorArgs = m_node.errorCall().sortedArguments();
		if (!errorArgs.empty())
			blob = awst::makeConcat(
				std::move(blob),
				eb::AbiEncoderBuilder::arc4EncodeArgsAtParamTypes(
					m_blk.builderCtx(), errorArgs,
					errorDef->functionType(true)->parameterTypes(), m_loc),
				m_loc);
		// Arg builds may hoist side effects / loop encoders to pre-effects.
		for (auto& pstmt: m_blk.builderCtx().takePreEffects())
			result.push_back(std::move(pstmt));
		result.push_back(makeRevertLogStmt(std::move(blob), m_loc));
	}

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

/// Bare `return;` — synthesize a return value from context.
void synthesizeBareReturnValue(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc, awst::ReturnStatement& stmt)
{
	auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(node.annotation());
	if (!retAnnotation.functionReturnParameters)
		return;
	auto const& retParams = retAnnotation.functionReturnParameters->parameters();
	// NOTE: bare `return;` in a function that HAS return parameters is
	// rejected by solc itself ("Return arguments required"), for both
	// single and multiple (named or not) returns — verified against
	// solc 0.8.20. So this branch only runs for a void function (no
	// return params), where retParams is empty and the arm below never
	// fires. The size()==1 synthesis is therefore effectively dead;
	// kept as-is. (fable-review-3 M2 was a false positive: the
	// frontend guards the shape, no null-value tuple can be emitted.)
	if (retParams.size() == 1)
	{
		auto* retType = blk.typeMapper().map(retParams[0]->type());
		if (!retParams[0]->name().empty())
			stmt.value = awst::makeVarExpression(retParams[0]->name(), retType, loc);
		else
			stmt.value = builder::StorageMapper::makeDefaultValue(retType, loc);
	}
}

/// `return <void expression>;` in a function with NO return values —
/// legal Solidity, and the shape forwarding wrappers use
/// (`return ctf.safeTransferFrom(…)` in Polymarket's NegRiskAdapter).
/// The expression must be EXECUTED as a statement: carrying it as the
/// return VALUE hands puya an inner-txn result handle where a stack
/// value belongs — "itxn_group_idx cannot be mapped to AVM stack type",
/// which reads as an unsupported feature and is really just a
/// misplaced expression.
bool tryVoidExprReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	auto const* voidRet = dynamic_cast<ReturnAnnotation const*>(&node.annotation());
	if (!voidRet || !voidRet->functionReturnParameters
		|| !voidRet->functionReturnParameters->parameters().empty())
		return false;

	auto call = blk.builderCtx().buildExpr(*node.expression());
	for (auto& p: blk.builderCtx().takePreEffects())
		result.push_back(std::move(p));
	if (call)
		result.push_back(awst::makeExpressionStatement(std::move(call), loc));
	for (auto& p: blk.builderCtx().takePostEffects())
		result.push_back(std::move(p));
	result.push_back(awst::makeReturnStatement(nullptr, loc));
	return true;
}

/// --evm-storage-layout: `return <storage expr>` in a function declared
/// `returns (T storage)` returns the biguint slot; multi-value returns with
/// storage components build component-wise. Returns true when the return was
/// consumed (including error early-outs, already logged).
bool trySlotStorageReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::shared_ptr<awst::ReturnStatement>& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!blk.typeMapper().profile().evmStorageLayout)
		return false;
	auto const* retAnn = dynamic_cast<ReturnAnnotation const*>(&node.annotation());
	if (!retAnn || !retAnn->functionReturnParameters)
		return false;

	auto const& rps = retAnn->functionReturnParameters->parameters();
	if (rps.size() == 1
		&& rps[0]->referenceLocation()
			== solidity::frontend::VariableDeclaration::Location::Storage)
	{
		EvmSlotLowering low(blk.builderCtx(), blk, loc);
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
	for (auto const& rp: rps)
		if (rp->referenceLocation()
			== solidity::frontend::VariableDeclaration::Location::Storage)
			anyStorageRet = true;
	if (rps.size() > 1 && anyStorageRet)
	{
		auto const* srcTup = dynamic_cast<
			solidity::frontend::TupleExpression const*>(
				node.expression());
		if (!srcTup
			|| srcTup->components().size() != rps.size())
		{
			Logger::instance().error(
				"--evm-storage-layout: multi-value return with "
				"storage refs must be a literal tuple", loc);
			return true;
		}
		EvmSlotLowering low(blk.builderCtx(), blk, loc);
		auto tup = awst::makeTupleExpression(nullptr, loc);
		std::vector<awst::WType const*> wts;
		for (size_t ri = 0; ri < rps.size(); ++ri)
		{
			auto const& compExpr = *srcTup->components()[ri];
			std::shared_ptr<awst::Expression> v;
			if (rps[ri]->referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::Storage)
			{
				auto addr = low.resolve(compExpr);
				if (!addr)
					return true;   // error already logged
				v = addr->slot;
			}
			else
			{
				v = blk.builderCtx().buildExpr(compExpr);
				if (v)
					v = builder::ConversionPlan{
						compExpr.annotation().type,
						rps[ri]->type(),
						blk.typeMapper().map(rps[ri]->type()),
						builder::ConversionPlan::Context::Return}.emit(
							std::move(v), loc);
			}
			if (!v)
				return true;
			wts.push_back(v->wtype);
			tup->items.push_back(std::move(v));
		}
		tup->wtype = blk.typeMapper()
			.createType<awst::WTuple>(std::move(wts), std::nullopt);
		stmt->value = std::move(tup);
		blk.builderCtx().appendEffectsTo(result);
		result.push_back(std::move(stmt));
		return true;
	}
	return false;
}

/// Box-keyed mapping-of-struct storage-ref RETURN, e.g.
/// `_getPool(id) -> Pool.State storage { return _pools[id]; }`. Return the
/// bytes box-key prefix of the indexed element (not its struct value); the
/// caller binds the result as a struct-storage-ref (SolVariableDeclaration),
/// and the return type is bytes (FunctionBuilder / mapReturnType).
bool tryBoxKeyedRefReturn(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::shared_ptr<awst::ReturnStatement>& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	bool storageRefMapReturn = false;
	if (auto const* retAnn =
			dynamic_cast<ReturnAnnotation const*>(&node.annotation()))
		if (retAnn->functionReturnParameters)
		{
			auto const& rps = retAnn->functionReturnParameters->parameters();
			if (rps.size() == 1
				&& rps[0]->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage
				&& builder::isBoxKeyedStorageRef(
					rps[0]->type(), blk.typeMapper().analysis())) // widened: plain structs too
				storageRefMapReturn = true;
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
void convertSingleReturnValue(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<ASTPointer<VariableDeclaration>> const& retParams,
	awst::ReturnStatement& stmt)
{
	auto const* targetSolType = retParams[0]->type();
	auto const* targetWType = blk.typeMapper().map(targetSolType);
	// Slot mode: `return <storage ref>` from a MEMORY-typed return
	// materializes the aggregate (the storage-declared return case
	// exited earlier with the raw slot).
	stmt.value = EvmSlotLowering::materializeRefValue(
		blk.builderCtx(), blk, std::move(stmt.value),
		node.expression()->annotation().type, targetWType, loc);
	stmt.value = builder::ConversionPlan{
		node.expression()->annotation().type,
		targetSolType,
		targetWType,
		builder::ConversionPlan::Context::Return}.emit(
			std::move(stmt.value), loc);
}

/// Multi-value declared return: coerce each tuple component to its declared
/// type (through a ternary's arms too).
void convertTupleReturnValue(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc,
	std::vector<ASTPointer<VariableDeclaration>> const& retParams,
	awst::ReturnStatement& stmt)
{
	std::vector<solidity::frontend::Type const*> sourceTypes;
	if (auto const* sourceTuple = dynamic_cast<
		solidity::frontend::TupleType const*>(
			node.expression()->annotation().type))
		sourceTypes.assign(
			sourceTuple->components().begin(),
			sourceTuple->components().end());

	// A tuple literal retains each component's pre-conversion source
	// annotation, which is more precise than the tuple's common type.
	if (auto const* sourceTupleExpr = dynamic_cast<
		solidity::frontend::TupleExpression const*>(
			node.expression()))
	{
		sourceTypes.clear();
		for (auto const& component: sourceTupleExpr->components())
			sourceTypes.push_back(
				component ? component->annotation().type : nullptr);
	}

	auto convertTuple = [&](awst::TupleExpression* tuple) {
		if (!tuple || tuple->items.size() != retParams.size())
			return;
		std::vector<awst::WType const*> targetTypes;
		for (size_t i = 0; i < retParams.size(); ++i)
		{
			auto const* targetSolType = retParams[i]->type();
			auto const* targetWType =
				blk.typeMapper().map(targetSolType);
			auto const* sourceSolType =
				i < sourceTypes.size() ? sourceTypes[i] : nullptr;
			tuple->items[i] = EvmSlotLowering::materializeRefValue(
				blk.builderCtx(), blk,
				std::move(tuple->items[i]), sourceSolType,
				targetWType, loc);
			tuple->items[i] = builder::ConversionPlan{
				sourceSolType,
				targetSolType,
				targetWType,
				builder::ConversionPlan::Context::Return}.emit(
					std::move(tuple->items[i]), loc);
			targetTypes.push_back(tuple->items[i]->wtype);
		}
		tuple->wtype = blk.typeMapper().createType<awst::WTuple>(
			std::move(targetTypes), std::nullopt);
	};

	if (auto* tuple = dynamic_cast<awst::TupleExpression*>(
		stmt.value.get()))
		convertTuple(tuple);
	else if (auto* conditional =
		dynamic_cast<awst::ConditionalExpression*>(stmt.value.get()))
	{
		convertTuple(dynamic_cast<awst::TupleExpression*>(
			conditional->trueExpr.get()));
		convertTuple(dynamic_cast<awst::TupleExpression*>(
			conditional->falseExpr.get()));
		if (conditional->trueExpr)
			conditional->wtype = conditional->trueExpr->wtype;
	}
}

/// Enum range validation on return: EVM panics (0x21) on invalid enum
/// return values.
void maybeAppendEnumReturnAssert(BlockContext& blk, Return const& node,
	awst::SourceLocation const& loc, awst::ReturnStatement& stmt,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!stmt.value)
		return;
	auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(node.annotation());
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

	if (!m_node.expression())
		synthesizeBareReturnValue(m_blk, m_node, m_loc, *stmt);
	else
	{
		if (tryVoidExprReturn(m_blk, m_node, m_loc, result))
			return result;
		if (trySlotStorageReturn(m_blk, m_node, m_loc, stmt, result))
			return result;
		if (tryBoxKeyedRefReturn(m_blk, m_node, m_loc, stmt, result))
			return result;

		stmt->value = m_blk.builderCtx().buildExpr(*m_node.expression());
		if (!stmt->value)
			return result;   // build errored (already logged) — don't deref

		// `return foo();` where foo is void: Solidity allows this when the
		// surrounding function is also void. The call must run for side effects;
		// the AWST return must carry no value (puya rejects void value
		// providers with 'Attempted to assign from expression that has no
		// result'). Solady's _revertWithPanic / SafeTransferLib internal
		// helpers tail-call other void functions this way.
		if (stmt->value->wtype == awst::WType::voidType())
		{
			result.push_back(awst::makeExpressionStatement(std::move(stmt->value), m_loc));
			stmt->value = nullptr;
			result.push_back(stmt);
			return result;
		}

		auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(m_node.annotation());
		if (retAnnotation.functionReturnParameters)
		{
			auto const& retParams = retAnnotation.functionReturnParameters->parameters();
			if (retParams.size() == 1)
				convertSingleReturnValue(m_blk, m_node, m_loc, retParams, *stmt);
			else if (retParams.size() > 1)
				convertTupleReturnValue(m_blk, m_node, m_loc, retParams, *stmt);
		}
	}

	m_blk.builderCtx().appendEffectsTo(result);

	maybeAppendEnumReturnAssert(m_blk, m_node, m_loc, *stmt, result);

	// D2 build-time ABI return encoding: wrap the (already value-coerced) return
	// value in its ABI wire type right here, instead of the ReturnRewriter post-pass
	// walking the finished body. Scalar + tuple (literal / ternary / opaque-spill).
	// Both the `return expr` and bare `return;`→named-var paths funnel through
	// stmt->value, so one call covers both. (sub-word mask / asm / modifier'd
	// returns still use the post-pass.)
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
