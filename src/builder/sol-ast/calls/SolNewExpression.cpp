/// @file SolNewExpression.cpp
/// new bytes(N), new T[](N), new Contract(...).
/// Migrated from FunctionCallBuilder.cpp lines 2144-2304.

#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
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

	auto e = std::make_shared<awst::NewArray>();
	e->sourceLocation = m_loc;
	e->wtype = resultType;

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
		// Try tracked constant locals
		if (n == 0)
		{
			if (auto const* ident = dynamic_cast<Identifier const*>(&*m_call.arguments()[0]))
			{
				auto it = m_ctx.constantLocals.find(
					ident->annotation().referencedDeclaration->id());
				if (it != m_ctx.constantLocals.end() && it->second > 0)
					n = it->second;
			}
		}

		if (n > 0)
		{
			// Compile-time known: N default values
			for (unsigned long long i = 0; i < n; ++i)
				e->values.push_back(
					builder::StorageMapper::makeDefaultValue(elemType, m_loc));
		}
		else
		{
			// Runtime-sized: loop pattern
			static int rtArrayCounter = 0;
			int tc = rtArrayCounter++;
			std::string arrName = "__rt_arr_" + std::to_string(tc);
			std::string idxName = "__rt_idx_" + std::to_string(tc);

			auto sizeExpr = buildExpr(*m_call.arguments()[0]);
			sizeExpr = builder::TypeCoercion::implicitNumericCast(
				std::move(sizeExpr), awst::WType::uint64Type(), m_loc);

			// __arr = NewArray()
			auto arrVar = awst::makeVarExpression(arrName, resultType, m_loc);

			auto initArr = awst::makeAssignmentStatement(arrVar, e, m_loc);
			m_ctx.prePendingStatements.push_back(std::move(initArr));

			// __i = 0
			auto idxVar = awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc);

			auto initIdx = std::make_shared<awst::AssignmentStatement>();
			initIdx->sourceLocation = m_loc;
			initIdx->target = idxVar;
			auto zero = awst::makeIntegerConstant("0", m_loc);
			initIdx->value = zero;
			m_ctx.prePendingStatements.push_back(std::move(initIdx));

			// while (__i < n)
			auto loop = std::make_shared<awst::WhileLoop>();
			loop->sourceLocation = m_loc;
			auto cond = awst::makeNumericCompare(idxVar, awst::NumericComparison::Lt, sizeExpr, m_loc);
			loop->condition = cond;

			auto loopBody = std::make_shared<awst::Block>();
			loopBody->sourceLocation = m_loc;

			// extend with default
			auto defaultElem = builder::StorageMapper::makeDefaultValue(elemType, m_loc);
			auto singleArr = std::make_shared<awst::NewArray>();
			singleArr->sourceLocation = m_loc;
			singleArr->wtype = resultType;
			singleArr->values.push_back(std::move(defaultElem));

			auto extend = std::make_shared<awst::ArrayExtend>();
			extend->sourceLocation = m_loc;
			extend->wtype = awst::WType::voidType();
			extend->base = arrVar;
			extend->other = std::move(singleArr);
			auto extendStmt = awst::makeExpressionStatement(extend, m_loc);
			loopBody->body.push_back(std::move(extendStmt));

			// __i++
			auto one = awst::makeIntegerConstant("1", m_loc);
			auto incr = awst::makeUInt64BinOp(idxVar, awst::UInt64BinaryOperator::Add, one, m_loc);
			auto incrAssign = awst::makeAssignmentStatement(idxVar, incr, m_loc);
			loopBody->body.push_back(std::move(incrAssign));

			loop->loopBody = loopBody;
			m_ctx.prePendingStatements.push_back(std::move(loop));

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

			// Determine whether the child needs a separate __postInit call
			// (contracts that read msg.value/sender/data in the ctor body or
			// base state initializers must run those at post-init time, not
			// at AppCreate time where the itxn sender/value are the parent).
			auto const* childCtor = contractType->contractDefinition().constructor();
			bool childHasPostInit = false;
			if (childCtor && childCtor->isImplemented())
			{
				struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
				{
					bool found = false;
					bool visit(solidity::frontend::MemberAccess const& _ma) override
					{
						if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_ma.expression()))
						{
							if (id->name() == "msg"
								&& (_ma.memberName() == "value"
									|| _ma.memberName() == "sender"
									|| _ma.memberName() == "data"))
								found = true;
						}
						return !found;
					}
				};
				MsgRefChecker checker;
				childCtor->body().accept(checker);
				auto const& lin = contractType->contractDefinition().annotation().linearizedBaseContracts;
				for (auto const* base: lin)
					for (auto const* var: base->stateVariables())
						if (var->value()) var->value()->accept(checker);
				childHasPostInit = checker.found;
			}

			// Build the ARC4-encoded ctor args once — reused either as
			// AppCreate's ApplicationArgs (no __postInit) or as the
			// __postInit selector+args tuple.
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

					if (argVal->wtype == awst::WType::biguintType())
					{
						unsigned bits = 256;
						auto const* intT = dynamic_cast<IntegerType const*>(paramSolType);
						if (!intT)
							if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(paramSolType))
								intT = dynamic_cast<IntegerType const*>(&udvt->underlyingType());
						if (intT && !intT->isSigned())
							bits = intT->numBits();
						auto* arc4T = m_ctx.typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
						auto encode = std::make_shared<awst::ARC4Encode>();
						encode->sourceLocation = m_loc;
						encode->wtype = arc4T;
						encode->value = std::move(argVal);
						argVal = std::move(encode);
					}
					else if (argVal->wtype == awst::WType::uint64Type())
					{
						unsigned bits = 64;
						auto const* intT = dynamic_cast<IntegerType const*>(paramSolType);
						if (intT) bits = intT->numBits();
						auto* arc4T = m_ctx.typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
						auto encode = std::make_shared<awst::ARC4Encode>();
						encode->sourceLocation = m_loc;
						encode->wtype = arc4T;
						encode->value = std::move(argVal);
						argVal = std::move(encode);
					}
					else if (argVal->wtype == awst::WType::boolType())
					{
						auto asU64 = awst::makeReinterpretCast(
							std::move(argVal), awst::WType::uint64Type(), m_loc);
						auto itob = awst::makeIntrinsicCall(
							"itob", awst::WType::bytesType(), m_loc);
						itob->stackArgs.push_back(std::move(asU64));
						argVal = std::move(itob);
					}
					out.push_back(std::move(argVal));
				}
				return out;
			};

			// Build inner appl create transaction with TemplateVar programs
			static awst::WInnerTransactionFields s_applFieldsType(6); // appl
			auto create = std::make_shared<awst::CreateInnerTransaction>();
			create->sourceLocation = m_loc;
			create->wtype = &s_applFieldsType;

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
			auto approvalTmpl = std::make_shared<awst::TemplateVar>();
			approvalTmpl->sourceLocation = m_loc;
			approvalTmpl->wtype = awst::WType::bytesType();
			approvalTmpl->name = "TMPL_APPROVAL_" + childName;
			create->fields["ApprovalProgram"] = std::move(approvalTmpl);

			// ClearStateProgram = TemplateVar("TMPL_CLEAR_ChildName")
			auto clearTmpl = std::make_shared<awst::TemplateVar>();
			clearTmpl->sourceLocation = m_loc;
			clearTmpl->wtype = awst::WType::bytesType();
			clearTmpl->name = "TMPL_CLEAR_" + childName;
			create->fields["ClearStateProgram"] = std::move(clearTmpl);

			// Child has no __postInit: its ctor runs during AppCreate, reading
			// ApplicationArgs[0..N-1] directly (same pattern as any ARC4
			// external-call routing but without a leading selector). Attach
			// the ARC4-encoded ctor args to the create itxn.
			if (!childHasPostInit && childCtor && !m_call.arguments().empty())
			{
				auto encodedArgs = buildEncodedCtorArgs();
				if (!encodedArgs.empty())
				{
					auto argsTuple = std::make_shared<awst::TupleExpression>();
					argsTuple->sourceLocation = m_loc;
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
			auto submit = std::make_shared<awst::SubmitInnerTransaction>();
			submit->sourceLocation = m_loc;
			submit->wtype = &s_applTxnType;
			submit->itxns.push_back(std::move(create));

			auto submitStmt = awst::makeExpressionStatement(std::move(submit), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(submitStmt));

			// Read CreatedApplicationID via itxn intrinsic and save to temp var
			// because subsequent fund txn would clobber the itxn context.
			auto createdAppIdCall = awst::makeIntrinsicCall("itxn", awst::WType::uint64Type(), m_loc);
			createdAppIdCall->immediates = {std::string("CreatedApplicationID")};

			static int newAppIdCounter = 0;
			std::string newAppIdVarName = "__new_app_id_" + std::to_string(newAppIdCounter++);
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
				auto fundAppParams = awst::makeIntrinsicCall("app_params_get", fundTupleType, m_loc);
				fundAppParams->immediates = {std::string("AppAddress")};
				fundAppParams->stackArgs.push_back(std::move(fundAppId));

				std::string fundTmpName = "__fund_app_result";
				auto fundTmpTarget = awst::makeVarExpression(fundTmpName, fundTupleType, m_loc);
				auto fundAssign = awst::makeAssignmentStatement(fundTmpTarget, std::move(fundAppParams), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(fundAssign));

				auto fundTupleRead = awst::makeVarExpression(fundTmpName, fundTupleType, m_loc);
				auto fundAddrBytes = std::make_shared<awst::TupleItemExpression>();
				fundAddrBytes->sourceLocation = m_loc;
				fundAddrBytes->wtype = awst::WType::bytesType();
				fundAddrBytes->base = std::move(fundTupleRead);
				fundAddrBytes->index = 0;
				auto fundAddr = awst::makeReinterpretCast(std::move(fundAddrBytes), awst::WType::accountType(), m_loc);

				static awst::WInnerTransactionFields s_fundFieldsType(1);
				auto fundCreate = std::make_shared<awst::CreateInnerTransaction>();
				fundCreate->sourceLocation = m_loc;
				fundCreate->wtype = &s_fundFieldsType;

				auto fundTypeVal = std::make_shared<awst::IntegerConstant>();
				fundTypeVal->sourceLocation = m_loc;
				fundTypeVal->wtype = awst::WType::uint64Type();
				fundTypeVal->value = "1"; // pay
				fundCreate->fields["TypeEnum"] = std::move(fundTypeVal);

				auto fundFee = awst::makeIntegerConstant("0", m_loc);
				fundCreate->fields["Fee"] = std::move(fundFee);

				fundCreate->fields["Receiver"] = std::move(fundAddr);

				// Base MBR (1M) + any `{value: N}` forwarded to the child ctor.
				// Solidity `new X{value: N}(...)` funds the child with N on
				// construction; AVM needs MBR on top. Bundling them into this
				// single pay itxn avoids a separate pay txn in the no-postInit
				// branch. In the postInit branch we still emit an additional
				// pay txn just before __postInit so msg.value resolves inside
				// the call (see below) — but the MBR + value is already live
				// on the child at that point, so the second pay is functionally
				// redundant w.r.t. balance and only serves to set msg.value.
				auto baseMbr = awst::makeIntegerConstant("1000000", m_loc);
				std::shared_ptr<awst::Expression> ctorValueForFund = extractCallValue();
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
				auto fundSubmit = std::make_shared<awst::SubmitInnerTransaction>();
				fundSubmit->sourceLocation = m_loc;
				fundSubmit->wtype = &s_fundTxnType;
				fundSubmit->itxns.push_back(std::move(fundCreate));

				auto fundStmt = awst::makeExpressionStatement(std::move(fundSubmit), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(fundStmt));
			}

			if (childHasPostInit)
			{
				// Build __postInit(t1,t2,...)void signature.
				auto solTypeToARC4Name = [this](Type const* _type) -> std::string {
					auto* mapped = m_ctx.typeMapper.map(_type);
					if (mapped == awst::WType::accountType())
						return "address";
					if (auto const* intT = dynamic_cast<IntegerType const*>(_type))
						return (intT->isSigned() ? "int" : "uint") + std::to_string(intT->numBits());
					auto* arc4Type = m_ctx.typeMapper.mapToARC4Type(mapped);
					return builder::TypeCoercion::wtypeToABIName(arc4Type);
				};

				std::string postInitSig = "__postInit(";
				bool first = true;
				for (auto const& p: childCtor->parameters())
				{
					if (!first) postInitSig += ",";
					postInitSig += solTypeToARC4Name(p->type());
					first = false;
				}
				postInitSig += ")void";

				auto methodConst = std::make_shared<awst::MethodConstant>();
				methodConst->sourceLocation = m_loc;
				methodConst->wtype = awst::WType::bytesType();
				methodConst->value = postInitSig;

				auto argsTuple = std::make_shared<awst::TupleExpression>();
				argsTuple->sourceLocation = m_loc;
				argsTuple->items.push_back(std::move(methodConst));

				auto encodedArgs = buildEncodedCtorArgs();
				for (auto& e: encodedArgs)
					argsTuple->items.push_back(std::move(e));

				// Set the TupleExpression's wtype to a WTuple over the item
				// types; without this, puya rejects the AWST (WInnerTxn
				// fields expect a WTuple, not the default void wtype).
				{
					std::vector<awst::WType const*> argTypes;
					for (auto const& item: argsTuple->items)
						argTypes.push_back(item->wtype);
					argsTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
						std::move(argTypes), std::nullopt);
				}

				// Payment group companion: sets msg.value inside __postInit.
				std::shared_ptr<awst::Expression> callValue = extractCallValue();
				if (!callValue)
					callValue = awst::makeIntegerConstant("0", m_loc);

				auto postAppId = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);

				// Re-read the app's address for the Payment receiver.
				auto* addrTupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
				auto postAddrCall = awst::makeIntrinsicCall("app_params_get", addrTupleType, m_loc);
				postAddrCall->immediates = {std::string("AppAddress")};
				postAddrCall->stackArgs.push_back(awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc));

				std::string addrTmp = "__postinit_addr_" + std::to_string(newAppIdCounter);
				auto addrTmpTarget = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
				auto addrAssign = awst::makeAssignmentStatement(addrTmpTarget, std::move(postAddrCall), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(addrAssign));

				auto addrRead = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
				auto addrBytes = std::make_shared<awst::TupleItemExpression>();
				addrBytes->sourceLocation = m_loc;
				addrBytes->wtype = awst::WType::bytesType();
				addrBytes->base = std::move(addrRead);
				addrBytes->index = 0;
				auto receiver = awst::makeReinterpretCast(std::move(addrBytes), awst::WType::accountType(), m_loc);

				// PaymentTxn (sets msg.value for __postInit)
				static awst::WInnerTransactionFields s_payFieldsType(1);
				auto payTxn = std::make_shared<awst::CreateInnerTransaction>();
				payTxn->sourceLocation = m_loc;
				payTxn->wtype = &s_payFieldsType;
				payTxn->fields["TypeEnum"] = awst::makeIntegerConstant("1", m_loc);
				payTxn->fields["Fee"] = awst::makeIntegerConstant("0", m_loc);
				payTxn->fields["Receiver"] = std::move(receiver);
				payTxn->fields["Amount"] = std::move(callValue);

				// AppCall __postInit(args)
				static awst::WInnerTransactionFields s_applFieldsType2(6);
				auto postCall = std::make_shared<awst::CreateInnerTransaction>();
				postCall->sourceLocation = m_loc;
				postCall->wtype = &s_applFieldsType2;
				postCall->fields["TypeEnum"] = awst::makeIntegerConstant("6", m_loc);
				postCall->fields["OnCompletion"] = awst::makeIntegerConstant("0", m_loc);
				postCall->fields["Fee"] = awst::makeIntegerConstant("0", m_loc);
				postCall->fields["ApplicationID"] = std::move(postAppId);
				postCall->fields["ApplicationArgs"] = std::move(argsTuple);

				// Submit as a group so the PaymentTxn is visible to __postInit's msg.value.
				static awst::WInnerTransaction s_payApplGroupType(1);
				auto postSubmit = std::make_shared<awst::SubmitInnerTransaction>();
				postSubmit->sourceLocation = m_loc;
				postSubmit->wtype = &s_payApplGroupType;
				postSubmit->itxns.push_back(std::move(payTxn));
				postSubmit->itxns.push_back(std::move(postCall));

				auto postStmt = awst::makeExpressionStatement(std::move(postSubmit), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(postStmt));
			}

			// Return CreatedApplicationID as applicationType directly.
			// This allows subsequent method calls to use the app ID
			// instead of converting through the hashed address.
			auto appIdCast = awst::makeReinterpretCast(std::move(createdAppId), awst::WType::applicationType(), m_loc);

			return appIdCast;
		}
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
