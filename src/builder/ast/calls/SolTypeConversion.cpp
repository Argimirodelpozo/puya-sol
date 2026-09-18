/// @file SolTypeConversion.cpp
/// Type conversion calls: uint256(x), address(y), bytes32(z), bool(w), etc.

#include "builder/ast/calls/SolTypeConversion.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"

#include "builder/storage/slot/EvmSlotLowering.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolTypeConversion::toAwst()
{
	if (m_call.arguments().empty())
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	// `address(this)` / `payable(this)` must yield the REAL application
	// address: bare `this` lowers to the contract-value FAKE form
	// (bzero(24) ++ itob(appId), see SolIdentifier), which is an app-id
	// carrier, not a payable/balance-bearing address.
	if (dynamic_cast<solidity::frontend::AddressType const*>(m_call.annotation().type)
		&& SolcFacts::isThis(*m_call.arguments()[0]))
		return awst::makeGlobal("CurrentApplicationAddress", awst::WType::accountType(), m_loc);

	// Enum range check: EnumType(x) must assert x < numMembers
	if (dynamic_cast<solidity::frontend::EnumType const*>(m_call.annotation().type))
		return handleEnumConversion();
	if (dynamic_cast<solidity::frontend::IntegerType const*>(m_call.annotation().type))
		return ConversionPlan{m_call.arguments()[0]->annotation().type,
			m_call.annotation().type, targetType, ConversionPlan::Context::ExplicitInteger}.emit(
				buildExpr(*m_call.arguments()[0]), m_loc);

	auto const* sourceType = m_call.arguments()[0]->annotation().type;
	auto const* solTarget = m_call.annotation().type;
	auto argument = buildExpr(*m_call.arguments()[0]);
	argument = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
		std::move(argument), sourceType, targetType, m_loc);
	// Solc has validated explicit conversion legality. Adapt only the target
	// carrier here; bytes20 addresses additionally need AVM's 32-byte width.
	using Category = solidity::frontend::Type::Category;
	switch (solTarget->category())
	{
	case Category::Address:
	case Category::Contract:
		if (sourceType->category() == Category::FixedBytes)
			return awst::makeAsAccount(awst::makeLeftPadToN(
				awst::makeAsBytes(std::move(argument), m_loc), 32, m_loc), m_loc);
		[[fallthrough]];
	case Category::Bool:
	case Category::FixedBytes:
		return TypeCoercion::coerceScalar(std::move(argument), targetType, m_loc);
	default: break;
	}
	if (sourceType->isImplicitlyConvertibleTo(*solTarget))
		return ConversionPlan{sourceType, solTarget, targetType, ConversionPlan::Context::Argument}
			.emit(std::move(argument), m_loc, &m_ctx.preEffects());

	// solc permits reference conversions only where implicitly convertible,
	// plus bytes <-> string views. It does not permit bytes-to-numeric arrays.
	auto const* sourceArray = dynamic_cast<solidity::frontend::ArrayType const*>(sourceType);
	if (auto const* slice = dynamic_cast<solidity::frontend::ArraySliceType const*>(sourceType))
		sourceArray = &slice->arrayType(); // solc delegates explicit conversion to the underlying array.
	auto const* targetArray = dynamic_cast<solidity::frontend::ArrayType const*>(solTarget);
	if (sourceArray && targetArray && sourceArray->isByteArrayOrString()
		&& targetArray->isByteArrayOrString())
		return awst::makeReinterpretCast(std::move(argument), targetType, m_loc);
	Logger::instance().error("Unsupported explicit conversion from "
		+ sourceType->toString() + " to " + solTarget->toString(), m_loc);
	return awst::makeVoidConstant(m_loc);
}

// ─────────────────────────────────────────────────────────────────────
// Enum conversion with range check
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::handleEnumConversion()
{
	auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(
		m_call.annotation().type);
	auto argExpr = buildExpr(*m_call.arguments()[0]);
	// Check the full word before narrowing, even for a discarded cast.
	argExpr = TypeCoercion::checkedEnum(std::move(argExpr), enumType, m_loc, &m_ctx.preEffects());
	return TypeCoercion::coerceScalar(std::move(argExpr), m_ctx.typeMapper.map(enumType), m_loc);
}

} // namespace puyasol::builder::sol_ast
