/// @file SolAddressProperty.cpp
/// address.code → app_params_get AppApprovalProgram.
/// Migrated from MemberAccessBuilder.cpp lines 582-655.

#include "builder/sol-ast/members/SolAddressProperty.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <variant>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAddressProperty::toAwst()
{
	std::string member = memberName();

	if (member == "code")
	{
		// address(N).code for literal N → empty bytes. Precompile/EOA
		// addresses have no code; app_params_get panics on non-existent app ids.
		{
			auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpression());
			if (fc && *fc->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
				&& fc->arguments().size() == 1)
			{
				auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(fc->arguments()[0].get());
				if (lit && lit->token() == solidity::frontend::Token::Number)
				{
					return awst::makeBytesConstant({}, m_loc);
				}
			}
		}

		auto addrExpr = buildExpr(baseExpression());

		// address(this) lowers to `global CurrentApplicationAddress`; swap in
		// CurrentApplicationID for app_params_get. Deriving app id from the
		// last 8 bytes of the address is unreliable across network configs.
		std::shared_ptr<awst::Expression> appId;
		if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(addrExpr.get()))
		{
			if (ic->opCode == "global"
				&& !ic->immediates.empty()
				&& std::holds_alternative<std::string>(ic->immediates[0])
				&& std::get<std::string>(ic->immediates[0]) == "CurrentApplicationAddress")
			{
				auto idCall = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), m_loc);
				auto cast = awst::makeAsApplication(std::move(idCall), m_loc);
				appId = std::move(cast);
			}
		}

		if (!appId)
		{
			// Arbitrary address → HARD ERROR: can't reliably map address→app id
			// across network configs. `address(this).code` and `address(N).code`
			// literals are handled above; stub appId so AWST building completes
			// (error aborts before TEAL is emitted).
			Logger::instance().error(
				"`address(addr).code` for a non-`this` address is not supported "
				"on AVM — an arbitrary address can't be reliably dereferenced to "
				"its application program. `address(this).code` is supported.", m_loc);
			auto idCall = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), m_loc);
			appId = awst::makeAsApplication(std::move(idCall), m_loc);
		}

		auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::bytesType(), awst::WType::boolType()});
		auto appParamsGet = awst::makeAppParamsGet(
			"AppApprovalProgram", std::move(appId), tupleType, m_loc);

		// Stash (bytes, bool) into a fresh temp before extracting the bytes elem.
		// puya's TupleItemExpression miscompiles pop ordering for a raw
		// IntrinsicCall; VarExpression works (same pattern as SolNewExpression).
		// Counter makes the name unique: two `.code` reads in one expression
		// would both read the second call's temp if they shared a name
		// (all prepends run before the returned expression is consumed).
		static int s_appProgramTmpCounter = 0;
		std::string tmpName =
			"__app_program_result_" + std::to_string(++s_appProgramTmpCounter);
		auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, m_loc);
		auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(appParamsGet), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assign));

		auto tupleRead = awst::makeVarExpression(tmpName, tupleType, m_loc);

		auto item = awst::makeTupleItem(std::move(tupleRead), 0, awst::WType::bytesType(), m_loc);
		return item;
	}

	if (member == "balance")
	{
		// address.balance → acct_params_get AcctBalance → uint64 → biguint
		Logger::instance().warning(
			"address.balance returns the account balance in microAlgos on AVM, "
			"not wei. 1 microAlgo = 1e-6 ALGO. This is NOT equivalent to EVM wei "
			"(1 wei = 1e-18 ETH). Ensure your contract logic accounts for this difference.", m_loc);

		// `address(contractExpr).balance`: SolTypeConversion builds a fake
		// (24-zero-pad ++ itob(app_id)) address; acct_params_get on it returns 0.
		// Resolve to the real address via app_params_get AppAddress instead.
		{
			auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpression());
			if (fc && *fc->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
				&& fc->arguments().size() == 1)
			{
				auto const* innerType = fc->arguments()[0]->annotation().type;
				bool isContractType = dynamic_cast<
					solidity::frontend::ContractType const*>(innerType) != nullptr;
				// Skip for `address(this)` — fallback below emits
				// `global CurrentApplicationAddress` directly.
				bool isThis = false;
				if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(
					fc->arguments()[0].get()))
				{
					if (id->name() == "this")
						isThis = true;
				}
				if (isContractType && !isThis)
				{
					auto appExpr = buildExpr(*fc->arguments()[0]);
					// Contract-typed identifiers lower to accountType (fake address);
					// reverse to app id. If already applicationType, reinterpret directly.
					std::shared_ptr<awst::Expression> appIdUint;
					if (appExpr->wtype == awst::WType::accountType())
					{
						auto appAsApp = TypeCoercion::coerceForAssignment(
							std::move(appExpr), awst::WType::applicationType(), m_loc);
						appIdUint = awst::makeAsUInt64(std::move(appAsApp), m_loc);
					}
					else
					{
						appIdUint = awst::makeAsUInt64(std::move(appExpr), m_loc);
					}
					auto* addrTupleType = m_ctx.typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{
							awst::WType::bytesType(), awst::WType::boolType()});
					auto appParamsGet = awst::makeAppParamsGet(
						"AppAddress", std::move(appIdUint), addrTupleType, m_loc);

					std::string addrTmp = "__app_balance_addr";
					auto addrTmpTarget = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
					auto addrAssign = awst::makeAssignmentStatement(
						addrTmpTarget, std::move(appParamsGet), m_loc);
					m_ctx.prePendingStatements.push_back(std::move(addrAssign));

					auto addrTupleRead = awst::makeVarExpression(addrTmp, addrTupleType, m_loc);
					auto addrBytesItem = awst::makeTupleItem(std::move(addrTupleRead), 0, awst::WType::bytesType(), m_loc);
					auto realAddr = awst::makeAsAccount(std::move(addrBytesItem), m_loc);

					auto* balTupleType = m_ctx.typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{
							awst::WType::uint64Type(), awst::WType::boolType()});
					auto acctParams = awst::makeIntrinsicCall(
						"acct_params_get", balTupleType, m_loc);
					acctParams->immediates = {std::string("AcctBalance")};
					acctParams->stackArgs.push_back(std::move(realAddr));

					auto bal = awst::makeTupleItem(std::move(acctParams), 0, awst::WType::uint64Type(), m_loc);

					auto itobBal = awst::makeItob(std::move(bal), m_loc);
					return awst::makeAsBiguint(std::move(itobBal), m_loc);
				}
			}
		}

		auto addrExpr = buildExpr(baseExpression());

		auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::uint64Type(), awst::WType::boolType()});
		auto acctParams = awst::makeIntrinsicCall("acct_params_get", tupleType, m_loc);
		acctParams->immediates = {std::string("AcctBalance")};
		acctParams->stackArgs.push_back(std::move(addrExpr));

		// Solidity balance is uint256 — promote uint64 → biguint
		auto balanceVal = awst::makeTupleItem(std::move(acctParams), 0, awst::WType::uint64Type(), m_loc);
		auto itob = awst::makeItob(std::move(balanceVal), m_loc);
		return awst::makeAsBiguint(std::move(itob), m_loc);
	}

	if (member == "codehash")
	{
		// address(this).codehash → keccak256(AppApprovalProgram).
		// Non-current addresses can't be cheaply resolved to an app id on AVM.
		// Literal bridge: address(0) → bytes32(0); address(1..10) → keccak256("") (no code, EVM convention).
		{
			auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpression());
			if (fc && *fc->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
				&& fc->arguments().size() == 1)
			{
				auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(fc->arguments()[0].get());
				if (lit && lit->token() == solidity::frontend::Token::Number)
				{
					auto litVal = lit->annotation().type->literalValue(lit);
					auto* w = m_ctx.typeMapper.createType<awst::BytesWType>(32);
					if (litVal == 0)
						return awst::makeBytesConstant(std::vector<uint8_t>(32, 0), m_loc, awst::BytesEncoding::Base16, w);
					return awst::makeBytesConstant(
						// keccak256 of empty bytes
						std::vector<uint8_t>{
							0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
							0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
							0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
							0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70},
						m_loc, awst::BytesEncoding::Base16, w);
				}
			}
		}
		auto addrExpr = buildExpr(baseExpression());
		if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(addrExpr.get()))
		{
			if (ic->opCode == "global"
				&& !ic->immediates.empty()
				&& std::holds_alternative<std::string>(ic->immediates[0])
				&& std::get<std::string>(ic->immediates[0]) == "CurrentApplicationAddress")
			{
				auto appId = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), m_loc);
				auto appIdCast = awst::makeAsApplication(std::move(appId), m_loc);

				auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{
						awst::WType::bytesType(), awst::WType::boolType()});
				auto appParamsGet = awst::makeAppParamsGet(
					"AppApprovalProgram", std::move(appIdCast), tupleType, m_loc);

				auto bytesOut = awst::makeTupleItem(std::move(appParamsGet), 0, awst::WType::bytesType(), m_loc);

				auto hash = awst::makeKeccak256(std::move(bytesOut), m_loc);

				if (m_wtype && m_wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(hash), m_wtype, m_loc);
					return cast;
				}
				return hash;
			}
		}
		// Arbitrary address → HARD ERROR. Old stub returned bytes32(0), which
		// silently corrupts codehash-based identity checks.
		Logger::instance().error(
			"`address(addr).codehash` for a non-`this` address is not supported "
			"on AVM — an arbitrary address can't be dereferenced to its code, so "
			"the old stub returned bytes32(0). `address(this).codehash` is "
			"supported.", m_loc);
		return awst::makeBytesConstant(
			std::vector<uint8_t>(32, 0), m_loc, awst::BytesEncoding::Base16,
			m_ctx.typeMapper.createType<awst::BytesWType>(32));
	}

	Logger::instance().warning("address property '." + member + "' has no Algorand equivalent", m_loc);
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
