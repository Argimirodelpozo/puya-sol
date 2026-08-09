/// @file SolNewExpression.cpp
/// new bytes(N), new T[](N), new Contract(...).
/// Migrated from FunctionCallBuilder.cpp lines 2144-2304.

#include "builder/sol-ast/calls/SolNewExpression.h"
#include "awst/NameGen.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder::sol_ast
{

std::set<std::string> SolNewExpression::s_childContracts;

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolNewExpression::handleNewBytes()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	auto sizeExpr = !m_call.arguments().empty()
		? buildExpr(*m_call.arguments()[0])
		: nullptr;
	if (sizeExpr)
		sizeExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(sizeExpr), awst::WType::uint64Type(), m_loc);

	auto e = awst::makeIntrinsicCall("bzero", resultType, m_loc);
	if (sizeExpr)
		e->stackArgs.push_back(std::move(sizeExpr));
	return e;
}

std::shared_ptr<awst::Expression> SolNewExpression::handleNewArray()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	awst::WType const* elemType = nullptr;
	if (auto* refArr = dynamic_cast<awst::ReferenceArray const*>(resultType))
		elemType = refArr->elementType();
	else if (auto* arc4Static = dynamic_cast<awst::ARC4StaticArray const*>(resultType))
		elemType = arc4Static->elementType();
	else if (auto* arc4Dyn = dynamic_cast<awst::ARC4DynamicArray const*>(resultType))
		elemType = arc4Dyn->elementType();

	auto e = awst::makeNewArray(resultType, m_loc);

	if (!m_call.arguments().empty() && elemType)
	{
		// Try compile-time size resolution
		unsigned long long n = 0;
		auto const* argType = m_call.arguments()[0]->annotation().type;
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(argType))
		{
			auto val = ratType->literalValue(nullptr);
			if (val > 0 && val <= 0xFFFF) // Reasonable compile-time array limit
				n = static_cast<unsigned long long>(val);
		}
		// `findConstantLocal` fold for Identifiers removed: never invalidated
		// on reassignment — silently folded loop counters to initial value
		// (hit by test_memory_arrays_of_various_sizes Pascal triangle).
		// Literals still fold via RationalNumberType.

		if (n > 0)
		{
			// `new bool[](N)`: bypass puya's ARC4 encoder (bug: setbit on empty
			// bytes → "index beyond byteslice"). Emit uint16(N)++bzero(ceil(N/8)).
			if (elemType == awst::WType::arc4BoolType()
				&& resultType->kind() == awst::WTypeKind::ARC4DynamicArray)
			{
				auto byteLen = static_cast<size_t>((n + 7) / 8);
				std::vector<uint8_t> data;
				data.reserve(2 + byteLen);
				data.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
				data.push_back(static_cast<uint8_t>(n & 0xFF));
				data.insert(data.end(), byteLen, 0);
				return awst::makeBytesConstant(
					std::move(data), m_loc, awst::BytesEncoding::Base16, resultType);
			}

			// Estimate encoded size: puya inlines as single pushbytes;
			// >4096 → rejects ("Invalid Bytes value"). Fall through to
			// runtime loop if over safety threshold.
			auto estimateEncodedSize = [](unsigned long long _n, awst::WType const* _resultType, awst::WType const* _elemType) -> uint64_t {
				uint64_t elemSize = 0;
				if (auto encoded = builder::arc4DefaultEncoding(_elemType))
					elemSize = encoded->size();
				bool elemIsDynamic = builder::arc4IsDynamic(_elemType);
				uint64_t headPerElem = elemIsDynamic ? 2 : elemSize;
				uint64_t tailPerElem = elemIsDynamic ? elemSize : 0;
				uint64_t outerHeader =
					_resultType->kind() == awst::WTypeKind::ARC4DynamicArray ? 2 : 0;
				return outerHeader + _n * headPerElem + _n * tailPerElem;
			};

			constexpr uint64_t kPushBytesSafetyLimit = 4000;
			if (estimateEncodedSize(n, resultType, elemType) <= kPushBytesSafetyLimit)
			{
				// Fits in pushbytes: emit N defaults.
				for (unsigned long long i = 0; i < n; ++i)
					e->values.push_back(
						builder::StorageMapper::makeDefaultValue(elemType, m_loc));
			}
			else
			{
				// Too large: fall through to runtime loop with literal size N.
				e->values.clear();
				auto fakeSizeExpr = awst::makeIntegerConstant(std::to_string(n), m_loc);
				int tc = awst::NameGen::next("SolNewExpression.rtArrayCounter");
				std::string arrName = "__rt_arr_" + std::to_string(tc);
				std::string idxName = "__rt_idx_" + std::to_string(tc);

				auto arrVar = awst::makeVarExpression(arrName, resultType, m_loc);
				m_ctx.prePendingStatements.push_back(
					awst::makeAssignmentStatement(arrVar, e, m_loc));

				auto idxVar = awst::makeVarExpression(
					idxName, awst::WType::uint64Type(), m_loc);
				m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
					idxVar, awst::makeIntegerConstant("0", m_loc), m_loc));

				auto cond = awst::makeNumericCompare(
					idxVar, awst::NumericComparison::Lt, fakeSizeExpr, m_loc);
				auto loopBody = awst::makeBlock(m_loc);

				auto defaultElem = builder::StorageMapper::makeDefaultValue(elemType, m_loc);
				auto singleArr = awst::makeNewArray(resultType, m_loc);
				singleArr->values.push_back(std::move(defaultElem));

				auto extend = awst::makeArrayExtend(arrVar, std::move(singleArr), m_loc);
				loopBody->body.push_back(awst::makeExpressionStatement(extend, m_loc));

				auto incr = awst::makeUInt64BinOp(idxVar, awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant("1", m_loc), m_loc);
				loopBody->body.push_back(awst::makeAssignmentStatement(idxVar, incr, m_loc));

				m_ctx.prePendingStatements.push_back(
					awst::makeWhileLoop(std::move(cond), std::move(loopBody), m_loc));

				return arrVar;
			}
		}
		else
		{
			// Runtime-sized: loop pattern
			int tc = awst::NameGen::next("SolNewExpression.rtArrayCounter");
			std::string arrName = "__rt_arr_" + std::to_string(tc);
			std::string idxName = "__rt_idx_" + std::to_string(tc);

			auto sizeExpr = buildExpr(*m_call.arguments()[0]);
			sizeExpr = builder::TypeCoercion::implicitNumericCast(
				std::move(sizeExpr), awst::WType::uint64Type(), m_loc);

			// Pin size to pre-loop temp: while-condition is re-evaluated each
			// iteration; `new T[](f())` would re-run f() otherwise. SingleEvaluation
			// materialises inside the loop header (still re-executes). Skip for
			// stable leaves (vars/constants).
			if (!dynamic_cast<awst::VarExpression const*>(sizeExpr.get())
				&& !dynamic_cast<awst::IntegerConstant const*>(sizeExpr.get()))
			{
				std::string sizeName = "__rt_size_" + std::to_string(tc);
				m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(sizeName, awst::WType::uint64Type(), m_loc),
					std::move(sizeExpr), m_loc));
				sizeExpr = awst::makeVarExpression(
					sizeName, awst::WType::uint64Type(), m_loc);
			}

			// `new bool[](n)` runtime: same puya bug as compile-time path.
			// Emit uint16(n)++bzero((n+7)/8) directly.
			if (elemType == awst::WType::arc4BoolType()
				&& resultType->kind() == awst::WTypeKind::ARC4DynamicArray)
			{
				auto sizeItob = awst::makeItob(sizeExpr, m_loc);
				auto lenHeader = awst::makeExtract(std::move(sizeItob), 6, 2, m_loc);
				auto plus7 = awst::makeUInt64BinOp(
					sizeExpr, awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant("7", m_loc), m_loc);
				auto byteLen = awst::makeUInt64BinOp(
					std::move(plus7), awst::UInt64BinaryOperator::FloorDiv,
					awst::makeIntegerConstant("8", m_loc), m_loc);
				auto bzero = awst::makeIntrinsicCall(
					"bzero", awst::WType::bytesType(), m_loc);
				bzero->stackArgs.push_back(std::move(byteLen));
				auto concat = awst::makeConcat(
					std::move(lenHeader), std::move(bzero), m_loc);
				return awst::makeReinterpretCast(
					std::move(concat), resultType, m_loc);
			}

			// __arr = NewArray()
			auto arrVar = awst::makeVarExpression(arrName, resultType, m_loc);

			auto initArr = awst::makeAssignmentStatement(arrVar, e, m_loc);
			m_ctx.prePendingStatements.push_back(std::move(initArr));

			// __i = 0
			auto idxVar = awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc);

			m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
				idxVar, awst::makeIntegerConstant("0", m_loc), m_loc));

			// while (__i < n)
			auto cond = awst::makeNumericCompare(idxVar, awst::NumericComparison::Lt, sizeExpr, m_loc);
			auto loopBody = awst::makeBlock(m_loc);

			// extend with default
			auto defaultElem = builder::StorageMapper::makeDefaultValue(elemType, m_loc);
			auto singleArr = awst::makeNewArray(resultType, m_loc);
			singleArr->values.push_back(std::move(defaultElem));

			auto extend = awst::makeArrayExtend(arrVar, std::move(singleArr), m_loc);
			loopBody->body.push_back(awst::makeExpressionStatement(extend, m_loc));

			// __i++
			auto incr = awst::makeUInt64BinOp(idxVar, awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant("1", m_loc), m_loc);
			loopBody->body.push_back(awst::makeAssignmentStatement(idxVar, incr, m_loc));

			m_ctx.prePendingStatements.push_back(
				awst::makeWhileLoop(std::move(cond), std::move(loopBody), m_loc));

			return arrVar;
		}
	}

	return e;
}

std::shared_ptr<awst::Expression> SolNewExpression::toAwst()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (resultType && resultType->kind() == awst::WTypeKind::Bytes)
		return handleNewBytes();

	// `new string(N)` allocates an N-byte string. Reuse the bytes handler
	// (which emits `bzero(N)`) and reinterpret the result as string.
	if (resultType == awst::WType::stringType())
	{
		auto sizeExpr = !m_call.arguments().empty()
			? buildExpr(*m_call.arguments()[0])
			: nullptr;
		if (sizeExpr)
			sizeExpr = builder::TypeCoercion::implicitNumericCast(
				std::move(sizeExpr), awst::WType::uint64Type(), m_loc);
		auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		if (sizeExpr)
			bzero->stackArgs.push_back(std::move(sizeExpr));
		auto cast = awst::makeReinterpretCast(std::move(bzero), resultType, m_loc);
		return cast;
	}

	if (resultType && (resultType->kind() == awst::WTypeKind::ReferenceArray
		|| resultType->kind() == awst::WTypeKind::ARC4StaticArray
		|| resultType->kind() == awst::WTypeKind::ARC4DynamicArray))
		return handleNewArray();

	// new Contract(...) — deploy child contract via inner app creation transaction.
	// Uses minimal stub programs since we can't embed the child's compiled bytecode
	// at this stage. The created app won't be functional but the address is valid.

	// `new C{salt:s}(...)` is CREATE2. CREATE2's address derivation (salt+initcode
	// hash) has no AVM equivalent — fail loud rather than silently wrong-lower.
	if (auto const* opts = dynamic_cast<FunctionCallOptions const*>(&m_call.expression()))
	{
		for (auto const& name : opts->names())
			if (name && *name == "salt")
			{
				Logger::instance().error(
					"`new C{salt: ...}(...)` (CREATE2) is not supported on AVM. "
					"CREATE2's deterministic address derivation (salt + initcode "
					"hash) has no AVM equivalent — app IDs are assigned "
					"sequentially by the chain at inner-app-create time, so a "
					"salt-derived address can't be pre-computed. Use plain "
					"`new C(...)` if you don't need address prediction.",
					m_loc);
				break;
			}
	}

	auto const& funcExpr = funcExpression();
	if (auto const* newExpr = dynamic_cast<NewExpression const*>(&funcExpr))
	{
		auto const* contractType = dynamic_cast<ContractType const*>(
			newExpr->typeName().annotation().type);
		if (contractType)
		{
			std::string childName = contractType->contractDefinition().name();
			Logger::instance().info(
				"'new " + childName + "()' — using template variables for "
				"child bytecode (substitute before deployment).");

			// Track this child contract for .tmpl file generation
			s_childContracts.insert(childName);

			// __postInit needed when ctor reads msg.value/sender/data (unavailable
			// at AppCreate time where sender/value belong to the parent).
			auto const* childCtor = contractType->contractDefinition().constructor();
			// THE postInit decision — the SAME computeNeedsPostInit the child compiles
			// with (PostInitTriggers: box writes, new C(), msg.*, AVM stdlib calls). This
			// used to be a local msg.*-only re-derivation that DRIFTED from the child:
			// a ctor writing a box-stored state var (dynamic array `s_ = s`) made the
			// child defer ALL init to __postInit while the caller passed args at create
			// and never called __postInit -> child deployed with NO state, failing only
			// on the first read (arrays_in_constructors).
			bool childHasPostInit = computeNeedsPostInit(contractType->contractDefinition());

			// ARC4-encode ctor args once; reused for AppCreate or __postInit.
			auto buildEncodedCtorArgs = [&]() {
				std::vector<std::shared_ptr<awst::Expression>> out;
				if (!childCtor) return out;
				auto const& ctorArgs = m_call.arguments();
				auto const& ctorParams = childCtor->parameters();
				for (size_t i = 0; i < ctorArgs.size() && i < ctorParams.size(); ++i)
				{
					auto argVal = buildExpr(*ctorArgs[i]);
					auto* paramSolType = ctorParams[i]->type();
					auto* paramWType = m_ctx.typeMapper.map(paramSolType);
					argVal = builder::TypeCoercion::implicitNumericCast(
						std::move(argVal), paramWType, m_loc);

					if (auto const* fixedB = dynamic_cast<FixedBytesType const*>(paramSolType))
					{
						// bytesN param: the callee decodes exactly N bytes (its
						// reader asserts the length). A hex-literal arg arrives
						// NUMERIC (uint64/biguint, leading zero bytes stripped)
						// — to bytes, then left-pad/trim to N.
						std::shared_ptr<awst::Expression> asB;
						if (argVal->wtype == awst::WType::uint64Type())
							asB = awst::makeItob(std::move(argVal), m_loc);
						else
							asB = awst::makeAsBytes(std::move(argVal), m_loc);
						argVal = awst::makeExtractLastN(
							awst::makeLeftPadToN(std::move(asB),
								static_cast<int>(fixedB->numBytes()), m_loc),
							static_cast<int>(fixedB->numBytes()), m_loc);
					}
					else if (argVal->wtype == awst::WType::biguintType())
					{
						auto it = builder::SolIntType::fromSol(paramSolType);
						if (childHasPostInit && (!it || it->isSigned))
						{
							// SIGNED params stay biguint in __postInit (arc56
							// renders them uint512; the router asserts 64
							// bytes) — send the canonical value zero-extended.
							argVal = awst::makeLeftPadToN(
								awst::makeAsBytes(std::move(argVal), m_loc), 64,
								m_loc);
						}
						else
						{
							unsigned bits = 256;
							if (it && !it->isSigned)
								bits = it->bits;
							auto* arc4T = m_ctx.typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
							auto encode = awst::makeARC4Encode(std::move(argVal), arc4T, m_loc);
							argVal = std::move(encode);
						}
					}
					else if (argVal->wtype == awst::WType::uint64Type())
					{
						if (childHasPostInit)
						{
							// __postInit keeps uint64-wtype params as declared
							// uint64 (router: btoi of an 8-byte arg).
							auto itob64 = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), m_loc);
							itob64->stackArgs.push_back(std::move(argVal));
							argVal = std::move(itob64);
						}
						else
						{
							unsigned bits = 64;
							auto const* intT = dynamic_cast<IntegerType const*>(paramSolType);
							if (intT) bits = intT->numBits();
							auto* arc4T = m_ctx.typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
							auto encode = awst::makeARC4Encode(std::move(argVal), arc4T, m_loc);
							argVal = std::move(encode);
						}
					}
					else if (argVal->wtype == awst::WType::boolType())
					{
						if (childHasPostInit)
						{
							// __postInit declares the param as arc4 bool (its
							// router asserts len==1); the 8-byte itob form is
							// the CREATE-path reader's convention only.
							argVal = awst::makeARC4Encode(std::move(argVal),
								awst::WType::arc4BoolType(), m_loc);
						}
						else
						{
							auto asU64 = awst::makeAsUInt64(std::move(argVal), m_loc);
							auto itob = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), m_loc);
							itob->stackArgs.push_back(std::move(asU64));
							argVal = std::move(itob);
						}
					}
					else if (argVal->wtype
						&& argVal->wtype->kind() == awst::WTypeKind::ReferenceArray)
					{
						// Aggregate ctor arg: the child's create/postInit reader expects the
						// ARC4 wire form (2-byte count header + elements — it reinterprets to
						// the arc4 type then ConvertArray's back). Encode the native array.
						auto const* arc4T = m_ctx.typeMapper.mapToARC4Type(argVal->wtype);
						if (arc4T != argVal->wtype)
							argVal = awst::makeARC4Encode(std::move(argVal), arc4T, m_loc);
					}
					out.push_back(std::move(argVal));
				}
				return out;
			};

			// Build inner appl create transaction with TemplateVar programs
			static awst::WInnerTransactionFields s_applFieldsType(6); // appl
			auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, m_loc);

			auto makeU64 = [&](std::string val) {
				auto c = awst::makeIntegerConstant(std::move(val), m_loc);
				return c;
			};
			create->fields["TypeEnum"] = makeU64("6");
			create->fields["Fee"] = makeU64("0");
			// Extra program pages for large child contracts
			create->fields["ExtraProgramPages"] = makeU64("3");
			// Global/local state schema — generous defaults
			create->fields["GlobalNumUint"] = makeU64("16");
			create->fields["GlobalNumByteSlice"] = makeU64("16");

			// ApprovalProgram = TemplateVar("TMPL_APPROVAL_ChildName")
			create->fields["ApprovalProgram"] = awst::makeTemplateVar(
				"TMPL_APPROVAL_" + childName, awst::WType::bytesType(), m_loc);

			// ClearStateProgram = TemplateVar("TMPL_CLEAR_ChildName")
			create->fields["ClearStateProgram"] = awst::makeTemplateVar(
				"TMPL_CLEAR_" + childName, awst::WType::bytesType(), m_loc);

			// No __postInit: ctor runs during AppCreate, reading ApplicationArgs[0..N-1].
			if (!childHasPostInit && childCtor && !m_call.arguments().empty())
			{
				auto encodedArgs = buildEncodedCtorArgs();
				if (!encodedArgs.empty())
				{
					auto argsTuple = awst::makeTupleExpression(nullptr, m_loc);
					std::vector<awst::WType const*> argTypes;
					for (auto& a: encodedArgs)
					{
						argTypes.push_back(a->wtype);
						argsTuple->items.push_back(std::move(a));
					}
					argsTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
						std::move(argTypes), std::nullopt);
					create->fields["ApplicationArgs"] = std::move(argsTuple);
				}
			}

			// Submit the inner transaction
			static awst::WInnerTransaction s_applTxnType(6);
			auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, m_loc);
			submit->itxns.push_back(std::move(create));

			auto submitStmt = awst::makeExpressionStatement(std::move(submit), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(submitStmt));

			// Read CreatedApplicationID via itxn intrinsic and save to temp var
			// because subsequent fund txn would clobber the itxn context.
			auto createdAppIdCall = awst::makeItxn(
				"CreatedApplicationID", awst::WType::uint64Type(), m_loc);

			int newAppId = awst::NameGen::next("SolNewExpression.newAppIdCounter");
			std::string newAppIdVarName = "__new_app_id_" + std::to_string(newAppId);
			auto newAppIdTarget = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);
			auto newAppIdAssign = awst::makeAssignmentStatement(newAppIdTarget, std::move(createdAppIdCall), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(newAppIdAssign));

			// Use the stored app ID from now on
			auto createdAppId = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);

			// Fund the newly created app with minimum balance (200000 microAlgos)
			{
				// Use the stored app ID
				auto fundAppId = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);

				auto* fundTupleType = new awst::WTuple(
					{awst::WType::bytesType(), awst::WType::boolType()});
				auto fundAppParams = awst::makeAppParamsGet(
					"AppAddress", std::move(fundAppId), fundTupleType, m_loc);

				std::string fundTmpName = "__fund_app_result";
				auto fundTmpTarget = awst::makeVarExpression(fundTmpName, fundTupleType, m_loc);
				auto fundAssign = awst::makeAssignmentStatement(fundTmpTarget, std::move(fundAppParams), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(fundAssign));

				auto fundTupleRead = awst::makeVarExpression(fundTmpName, fundTupleType, m_loc);
				auto fundAddrBytes = awst::makeTupleItem(std::move(fundTupleRead), 0, awst::WType::bytesType(), m_loc);
				auto fundAddr = awst::makeAsAccount(std::move(fundAddrBytes), m_loc);

				static awst::WInnerTransactionFields s_fundFieldsType(1);
				auto fundCreate = awst::makeCreateInnerTransaction(&s_fundFieldsType, m_loc);

				fundCreate->fields["TypeEnum"] = awst::makeOne(m_loc); // pay

				auto fundFee = awst::makeZero(m_loc);
				fundCreate->fields["Fee"] = std::move(fundFee);

				fundCreate->fields["Receiver"] = std::move(fundAddr);

				// MBR (1M) + value ONLY when no __postInit: with postInit, value
				// travels in the [pay(value),__postInit] group (gtxns Amount GI-1).
				// Bundling here too → 2x value (MBR+2*500000 verified). A pay txn
				// always transfers Amount — no "only-sets-msg.value" mode.
				// [pay,create,postInit] impossible: child's addr/app-id unknown until create.
				auto baseMbr = awst::makeIntegerConstant("1000000", m_loc);
				std::shared_ptr<awst::Expression> ctorValueForFund =
					childHasPostInit ? nullptr : extractCallValue();
				std::shared_ptr<awst::Expression> totalFundAmount;
				if (ctorValueForFund)
				{
					totalFundAmount = awst::makeUInt64BinOp(
						std::move(baseMbr), awst::UInt64BinaryOperator::Add,
						std::move(ctorValueForFund), m_loc);
				}
				else
				{
					totalFundAmount = std::move(baseMbr);
				}
				fundCreate->fields["Amount"] = std::move(totalFundAmount);

				static awst::WInnerTransaction s_fundTxnType(1);
				auto fundSubmit = awst::makeSubmitInnerTransaction(&s_fundTxnType, m_loc);
				fundSubmit->itxns.push_back(std::move(fundCreate));

				auto fundStmt = awst::makeExpressionStatement(std::move(fundSubmit), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(fundStmt));
			}

			if (childHasPostInit)
			{
				// Build __postInit(t1,t2,...)void signature via THE shared
				// top-level param namer (eb::solTypeToArc4ParamName — enums
				// collapse to their uint64 carrier, exactly what the callee
				// publishes; nestedArc4Name would say uint8 and mis-selector).
				// Replaces a local twin lacking enum/UDVT/bytesN/aggregate
				// handling (T4 twin drift; possible_solc item 4 scope: wire
				// sigs must mirror PUYA's wtype-derived naming, never solc's
				// EVM-canonical spelling).
				std::string postInitSig = "__postInit(";
				bool first = true;
				// ctor-less child can still need __postInit (box state-var initializers).
				std::vector<solidity::frontend::ASTPointer<solidity::frontend::VariableDeclaration>> const noParams;
				for (auto const& p: childCtor ? childCtor->parameters() : noParams)
				{
					if (!first) postInitSig += ",";
					postInitSig += eb::solTypeToArc4ParamName(m_ctx, p->type());
					first = false;
				}
				postInitSig += ")void";

				auto methodConst = awst::makeMethodConstant(
					postInitSig, awst::WType::bytesType(), m_loc);

				auto argsTuple = awst::makeTupleExpression(nullptr, m_loc);
				argsTuple->items.push_back(std::move(methodConst));

				auto encodedArgs = buildEncodedCtorArgs();
				for (auto& e: encodedArgs)
					argsTuple->items.push_back(std::move(e));

				// wtype required: puya rejects WInnerTxn ApplicationArgs with void wtype.
				{
					std::vector<awst::WType const*> argTypes;
					for (auto const& item: argsTuple->items)
						argTypes.push_back(item->wtype);
					argsTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
						std::move(argTypes), std::nullopt);
				}

				// Payment txn: sets msg.value for __postInit.
				std::shared_ptr<awst::Expression> callValue = extractCallValue();
				if (!callValue)
					callValue = awst::makeZero(m_loc);

				auto postAppId = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);

				// Re-read the app's address for the Payment receiver.
				auto* addrTupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
				auto postAddrCall = awst::makeAppParamsGet(
					"AppAddress",
					awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc),
					addrTupleType, m_loc);

				// (was a bare read of the post-incremented static → id + 1)
				std::string addrTmp = "__postinit_addr_" + std::to_string(newAppId + 1);
				auto addrTmpTarget = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
				auto addrAssign = awst::makeAssignmentStatement(addrTmpTarget, std::move(postAddrCall), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(addrAssign));

				auto addrRead = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
				auto addrBytes = awst::makeTupleItem(std::move(addrRead), 0, awst::WType::bytesType(), m_loc);
				auto receiver = awst::makeAsAccount(std::move(addrBytes), m_loc);

				// PaymentTxn (sets msg.value for __postInit)
				static awst::WInnerTransactionFields s_payFieldsType(1);
				auto payTxn = awst::makeCreateInnerTransaction(&s_payFieldsType, m_loc);
				payTxn->fields["TypeEnum"] = awst::makeOne(m_loc);
				payTxn->fields["Fee"] = awst::makeZero(m_loc);
				payTxn->fields["Receiver"] = std::move(receiver);
				payTxn->fields["Amount"] = std::move(callValue);

				// AppCall __postInit(args)
				static awst::WInnerTransactionFields s_applFieldsType2(6);
				auto postCall = awst::makeCreateInnerTransaction(&s_applFieldsType2, m_loc);
				postCall->fields["TypeEnum"] = awst::makeIntegerConstant("6", m_loc);
				postCall->fields["OnCompletion"] = awst::makeZero(m_loc);
				postCall->fields["Fee"] = awst::makeZero(m_loc);
				postCall->fields["ApplicationID"] = std::move(postAppId);
				postCall->fields["ApplicationArgs"] = std::move(argsTuple);

				// Group: PaymentTxn must be visible to __postInit's msg.value.
				static awst::WInnerTransaction s_payApplGroupType(1);
				auto postSubmit = awst::makeSubmitInnerTransaction(&s_payApplGroupType, m_loc);
				postSubmit->itxns.push_back(std::move(payTxn));
				postSubmit->itxns.push_back(std::move(postCall));

				auto postStmt = awst::makeExpressionStatement(std::move(postSubmit), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(postStmt));
			}

			// Return as applicationType (avoids address-hash conversion for calls).
			auto appIdCast = awst::makeAsApplication(std::move(createdAppId), m_loc);

			return appIdCast;
		}
	}

	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

} // namespace puyasol::builder::sol_ast
