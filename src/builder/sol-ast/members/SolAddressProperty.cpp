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
		// Compile-time literal bridge: address(N).code for integer N
		// returns empty bytes. Precompile addresses (1..10) and EOAs
		// have no code on EVM. Skip the app_params_get path for these
		// since it panics with "unavailable App N" on non-existent
		// app ids.
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
				auto cast = awst::makeReinterpretCast(std::move(idCall), awst::WType::applicationType(), m_loc);
				appId = std::move(cast);
			}
		}

		if (!appId)
		{
			std::shared_ptr<awst::Expression> bytesExpr = std::move(addrExpr);
			if (bytesExpr->wtype == awst::WType::accountType())
			{
				auto toBytes = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), m_loc);
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

			auto castId = awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), m_loc);
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
		auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, m_loc);
		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = m_loc;
		assign->target = tmpTarget;
		assign->value = std::move(appParamsGet);
		m_ctx.prePendingStatements.push_back(std::move(assign));

		auto tupleRead = awst::makeVarExpression(tmpName, tupleType, m_loc);

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

		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		return cast;
	}

	if (member == "codehash")
	{
		// Solidity: address.codehash == keccak256 of the account's code,
		// or bytes32(0) for EOAs / non-existent addresses.
		//
		// On AVM we can fetch the current app's approval program via
		// `app_params_get AppApprovalProgram (global CurrentApplicationID)`
		// and hash it with keccak256. For non-current addresses there is
		// no cheap way to dereference the address → app id.
		//
		// Compile-time literal bridge: if the base expression is a
		// FunctionCall that wraps an integer literal in `address(...)`,
		// resolve it at compile time. address(0) → 0, small non-zero
		// addresses → keccak256("") (matches EVM precompile convention
		// where addresses 1..10 have no code), larger literals → 0.
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
				auto appId = std::make_shared<awst::IntrinsicCall>();
				appId->sourceLocation = m_loc;
				appId->wtype = awst::WType::uint64Type();
				appId->opCode = "global";
				appId->immediates = {std::string("CurrentApplicationID")};
				auto appIdCast = awst::makeReinterpretCast(std::move(appId), awst::WType::applicationType(), m_loc);

				auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{
						awst::WType::bytesType(), awst::WType::boolType()});
				auto appParamsGet = std::make_shared<awst::IntrinsicCall>();
				appParamsGet->sourceLocation = m_loc;
				appParamsGet->wtype = tupleType;
				appParamsGet->opCode = "app_params_get";
				appParamsGet->immediates = {std::string("AppApprovalProgram")};
				appParamsGet->stackArgs.push_back(std::move(appIdCast));

				auto bytesOut = std::make_shared<awst::TupleItemExpression>();
				bytesOut->sourceLocation = m_loc;
				bytesOut->wtype = awst::WType::bytesType();
				bytesOut->base = std::move(appParamsGet);
				bytesOut->index = 0;

				auto hash = std::make_shared<awst::IntrinsicCall>();
				hash->sourceLocation = m_loc;
				hash->wtype = awst::WType::bytesType();
				hash->opCode = "keccak256";
				hash->stackArgs.push_back(std::move(bytesOut));

				if (m_wtype && m_wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(hash), m_wtype, m_loc);
					return cast;
				}
				return hash;
			}
		}
		// Fallback: bytes32(0) for non-this addresses.
		Logger::instance().warning(
			"address(other).codehash returns bytes32(0) on AVM — no way to "
			"dereference an arbitrary address to its application code.", m_loc);
		return awst::makeBytesConstant(
			std::vector<uint8_t>(32, 0), m_loc, awst::BytesEncoding::Base16,
			m_ctx.typeMapper.createType<awst::BytesWType>(32));
	}

	Logger::instance().warning("address property '." + member + "' has no Algorand equivalent", m_loc);
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
