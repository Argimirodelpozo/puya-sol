/// @file SolNewExpression.cpp
/// new bytes(N), new T[](N), new Contract(...).
/// Migrated from FunctionCallBuilder.cpp lines 2144-2304.

#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

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

	auto e = std::make_shared<awst::IntrinsicCall>();
	e->sourceLocation = m_loc;
	e->wtype = resultType;
	e->opCode = "bzero";
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
			auto arrVar = std::make_shared<awst::VarExpression>();
			arrVar->sourceLocation = m_loc;
			arrVar->wtype = resultType;
			arrVar->name = arrName;

			auto initArr = std::make_shared<awst::AssignmentStatement>();
			initArr->sourceLocation = m_loc;
			initArr->target = arrVar;
			initArr->value = e;
			m_ctx.prePendingStatements.push_back(std::move(initArr));

			// __i = 0
			auto idxVar = std::make_shared<awst::VarExpression>();
			idxVar->sourceLocation = m_loc;
			idxVar->wtype = awst::WType::uint64Type();
			idxVar->name = idxName;

			auto initIdx = std::make_shared<awst::AssignmentStatement>();
			initIdx->sourceLocation = m_loc;
			initIdx->target = idxVar;
			auto zero = awst::makeIntegerConstant("0", m_loc);
			initIdx->value = zero;
			m_ctx.prePendingStatements.push_back(std::move(initIdx));

			// while (__i < n)
			auto loop = std::make_shared<awst::WhileLoop>();
			loop->sourceLocation = m_loc;
			auto cond = std::make_shared<awst::NumericComparisonExpression>();
			cond->sourceLocation = m_loc;
			cond->wtype = awst::WType::boolType();
			cond->lhs = idxVar;
			cond->op = awst::NumericComparison::Lt;
			cond->rhs = sizeExpr;
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
			auto extendStmt = std::make_shared<awst::ExpressionStatement>();
			extendStmt->sourceLocation = m_loc;
			extendStmt->expr = extend;
			loopBody->body.push_back(std::move(extendStmt));

			// __i++
			auto one = awst::makeIntegerConstant("1", m_loc);
			auto incr = std::make_shared<awst::UInt64BinaryOperation>();
			incr->sourceLocation = m_loc;
			incr->wtype = awst::WType::uint64Type();
			incr->left = idxVar;
			incr->op = awst::UInt64BinaryOperator::Add;
			incr->right = one;
			auto incrAssign = std::make_shared<awst::AssignmentStatement>();
			incrAssign->sourceLocation = m_loc;
			incrAssign->target = idxVar;
			incrAssign->value = incr;
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
		auto bzero = std::make_shared<awst::IntrinsicCall>();
		bzero->sourceLocation = m_loc;
		bzero->wtype = awst::WType::bytesType();
		bzero->opCode = "bzero";
		if (sizeExpr)
			bzero->stackArgs.push_back(std::move(sizeExpr));
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = resultType;
		cast->expr = std::move(bzero);
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

			// Submit the inner transaction
			static awst::WInnerTransaction s_applTxnType(6);
			auto submit = std::make_shared<awst::SubmitInnerTransaction>();
			submit->sourceLocation = m_loc;
			submit->wtype = &s_applTxnType;
			submit->itxns.push_back(std::move(create));

			auto submitStmt = std::make_shared<awst::ExpressionStatement>();
			submitStmt->sourceLocation = m_loc;
			submitStmt->expr = std::move(submit);
			m_ctx.prePendingStatements.push_back(std::move(submitStmt));

			// Read CreatedApplicationID via itxn intrinsic and save to temp var
			// because subsequent fund txn would clobber the itxn context.
			auto createdAppIdCall = std::make_shared<awst::IntrinsicCall>();
			createdAppIdCall->sourceLocation = m_loc;
			createdAppIdCall->wtype = awst::WType::uint64Type();
			createdAppIdCall->opCode = "itxn";
			createdAppIdCall->immediates = {std::string("CreatedApplicationID")};

			static int newAppIdCounter = 0;
			std::string newAppIdVarName = "__new_app_id_" + std::to_string(newAppIdCounter++);
			auto newAppIdTarget = std::make_shared<awst::VarExpression>();
			newAppIdTarget->sourceLocation = m_loc;
			newAppIdTarget->name = newAppIdVarName;
			newAppIdTarget->wtype = awst::WType::uint64Type();
			auto newAppIdAssign = std::make_shared<awst::AssignmentStatement>();
			newAppIdAssign->sourceLocation = m_loc;
			newAppIdAssign->target = newAppIdTarget;
			newAppIdAssign->value = std::move(createdAppIdCall);
			m_ctx.prePendingStatements.push_back(std::move(newAppIdAssign));

			// Use the stored app ID from now on
			auto createdAppId = std::make_shared<awst::VarExpression>();
			createdAppId->sourceLocation = m_loc;
			createdAppId->name = newAppIdVarName;
			createdAppId->wtype = awst::WType::uint64Type();

			// Fund the newly created app with minimum balance (200000 microAlgos)
			{
				// Use the stored app ID
				auto fundAppId = std::make_shared<awst::VarExpression>();
				fundAppId->sourceLocation = m_loc;
				fundAppId->name = newAppIdVarName;
				fundAppId->wtype = awst::WType::uint64Type();

				auto* fundTupleType = new awst::WTuple(
					{awst::WType::bytesType(), awst::WType::boolType()});
				auto fundAppParams = std::make_shared<awst::IntrinsicCall>();
				fundAppParams->sourceLocation = m_loc;
				fundAppParams->wtype = fundTupleType;
				fundAppParams->opCode = "app_params_get";
				fundAppParams->immediates = {std::string("AppAddress")};
				fundAppParams->stackArgs.push_back(std::move(fundAppId));

				std::string fundTmpName = "__fund_app_result";
				auto fundTmpTarget = std::make_shared<awst::VarExpression>();
				fundTmpTarget->sourceLocation = m_loc;
				fundTmpTarget->name = fundTmpName;
				fundTmpTarget->wtype = fundTupleType;
				auto fundAssign = std::make_shared<awst::AssignmentStatement>();
				fundAssign->sourceLocation = m_loc;
				fundAssign->target = fundTmpTarget;
				fundAssign->value = std::move(fundAppParams);
				m_ctx.prePendingStatements.push_back(std::move(fundAssign));

				auto fundTupleRead = std::make_shared<awst::VarExpression>();
				fundTupleRead->sourceLocation = m_loc;
				fundTupleRead->name = fundTmpName;
				fundTupleRead->wtype = fundTupleType;
				auto fundAddrBytes = std::make_shared<awst::TupleItemExpression>();
				fundAddrBytes->sourceLocation = m_loc;
				fundAddrBytes->wtype = awst::WType::bytesType();
				fundAddrBytes->base = std::move(fundTupleRead);
				fundAddrBytes->index = 0;
				auto fundAddr = std::make_shared<awst::ReinterpretCast>();
				fundAddr->sourceLocation = m_loc;
				fundAddr->wtype = awst::WType::accountType();
				fundAddr->expr = std::move(fundAddrBytes);

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

				auto fundAmount = std::make_shared<awst::IntegerConstant>();
				fundAmount->sourceLocation = m_loc;
				fundAmount->wtype = awst::WType::uint64Type();
				fundAmount->value = "1000000"; // generous MBR for child + extra pages + state
				fundCreate->fields["Amount"] = std::move(fundAmount);

				static awst::WInnerTransaction s_fundTxnType(1);
				auto fundSubmit = std::make_shared<awst::SubmitInnerTransaction>();
				fundSubmit->sourceLocation = m_loc;
				fundSubmit->wtype = &s_fundTxnType;
				fundSubmit->itxns.push_back(std::move(fundCreate));

				auto fundStmt = std::make_shared<awst::ExpressionStatement>();
				fundStmt->sourceLocation = m_loc;
				fundStmt->expr = std::move(fundSubmit);
				m_ctx.prePendingStatements.push_back(std::move(fundStmt));
			}

			// Return CreatedApplicationID as applicationType directly.
			// This allows subsequent method calls to use the app ID
			// instead of converting through the hashed address.
			auto appIdCast = std::make_shared<awst::ReinterpretCast>();
			appIdCast->sourceLocation = m_loc;
			appIdCast->wtype = awst::WType::applicationType();
			appIdCast->expr = std::move(createdAppId);

			return appIdCast;
		}
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
