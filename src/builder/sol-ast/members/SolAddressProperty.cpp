/// @file SolAddressProperty.cpp
/// address.code → app_params_get AppApprovalProgram.
/// Migrated from MemberAccessBuilder.cpp lines 582-655.

#include "builder/sol-ast/members/SolAddressProperty.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <variant>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAddressProperty::toAwst()
{
	std::string member = memberName();

	if (member == "code")
	{
		auto addrExpr = buildExpr(baseExpression());

		// Fast path: if the receiver is `address(this)` — which AWSTBuilder
		// lowers to `global CurrentApplicationAddress` — swap in
		// `global CurrentApplicationID` so `app_params_get` gets the right
		// app id. Deriving the id from the last 8 bytes of the address only
		// works for some app-derived addresses and breaks under different
		// network configurations.
		std::shared_ptr<awst::Expression> appId;
		if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(addrExpr.get()))
		{
			if (ic->opCode == "global"
				&& !ic->immediates.empty()
				&& std::holds_alternative<std::string>(ic->immediates[0])
				&& std::get<std::string>(ic->immediates[0]) == "CurrentApplicationAddress")
			{
				auto idCall = std::make_shared<awst::IntrinsicCall>();
				idCall->sourceLocation = m_loc;
				idCall->wtype = awst::WType::uint64Type();
				idCall->opCode = "global";
				idCall->immediates = {std::string("CurrentApplicationID")};
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = m_loc;
				cast->wtype = awst::WType::applicationType();
				cast->expr = std::move(idCall);
				appId = std::move(cast);
			}
		}

		if (!appId)
		{
			std::shared_ptr<awst::Expression> bytesExpr = std::move(addrExpr);
			if (bytesExpr->wtype == awst::WType::accountType())
			{
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = m_loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(bytesExpr);
				bytesExpr = std::move(toBytes);
			}

			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = m_loc;
			extract->wtype = awst::WType::bytesType();
			extract->opCode = "extract";
			extract->immediates = {24, 8};
			extract->stackArgs.push_back(std::move(bytesExpr));

			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = m_loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
			btoi->stackArgs.push_back(std::move(extract));

			auto castId = std::make_shared<awst::ReinterpretCast>();
			castId->sourceLocation = m_loc;
			castId->wtype = awst::WType::applicationType();
			castId->expr = std::move(btoi);
			appId = std::move(castId);
		}

		auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::bytesType(), awst::WType::boolType()});
		auto appParamsGet = std::make_shared<awst::IntrinsicCall>();
		appParamsGet->sourceLocation = m_loc;
		appParamsGet->wtype = tupleType;
		appParamsGet->opCode = "app_params_get";
		appParamsGet->immediates = {std::string("AppApprovalProgram")};
		appParamsGet->stackArgs.push_back(std::move(appId));

		// Stash the (bytes, bool) tuple into a fresh temp before pulling
		// out the bytes element. Puya's TupleItemExpression lowering for a
		// raw IntrinsicCall miscompiles the pop ordering of app_params_get;
		// going through a VarExpression matches the pattern that
		// SolNewExpression already uses successfully.
		std::string tmpName = "__app_program_result";
		auto tmpTarget = std::make_shared<awst::VarExpression>();
		tmpTarget->sourceLocation = m_loc;
		tmpTarget->name = tmpName;
		tmpTarget->wtype = tupleType;
		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = m_loc;
		assign->target = tmpTarget;
		assign->value = std::move(appParamsGet);
		m_ctx.prePendingStatements.push_back(std::move(assign));

		auto tupleRead = std::make_shared<awst::VarExpression>();
		tupleRead->sourceLocation = m_loc;
		tupleRead->name = tmpName;
		tupleRead->wtype = tupleType;

		auto item = std::make_shared<awst::TupleItemExpression>();
		item->sourceLocation = m_loc;
		item->wtype = awst::WType::bytesType();
		item->base = std::move(tupleRead);
		item->index = 0;
		return item;
	}

	if (member == "balance")
	{
		// address.balance → acct_params_get AcctBalance → uint64 → biguint
		Logger::instance().warning(
			"address.balance returns the account balance in microAlgos on AVM, "
			"not wei. 1 microAlgo = 1e-6 ALGO. This is NOT equivalent to EVM wei "
			"(1 wei = 1e-18 ETH). Ensure your contract logic accounts for this difference.", m_loc);
		auto addrExpr = buildExpr(baseExpression());

		// acct_params_get AcctBalance returns (uint64, bool)
		auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::uint64Type(), awst::WType::boolType()});
		auto acctParams = std::make_shared<awst::IntrinsicCall>();
		acctParams->sourceLocation = m_loc;
		acctParams->wtype = tupleType;
		acctParams->opCode = "acct_params_get";
		acctParams->immediates = {std::string("AcctBalance")};
		acctParams->stackArgs.push_back(std::move(addrExpr));

		// Extract the balance (index 0)
		auto balanceVal = std::make_shared<awst::TupleItemExpression>();
		balanceVal->sourceLocation = m_loc;
		balanceVal->wtype = awst::WType::uint64Type();
		balanceVal->base = std::move(acctParams);
		balanceVal->index = 0;

		// Solidity returns uint256 for balance — promote uint64 → biguint
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = m_loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(balanceVal));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	}

	Logger::instance().warning("address property '." + member + "' has no Algorand equivalent", m_loc);
	auto e = std::make_shared<awst::BytesConstant>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::bytesType();
	e->encoding = awst::BytesEncoding::Base16;
	e->value = {};
	return e;
}

} // namespace puyasol::builder::sol_ast
