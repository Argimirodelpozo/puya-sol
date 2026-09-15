/// @file SolTypeConversion.cpp
/// Type conversion calls: uint256(x), address(y), bytes32(z), bool(w), etc.

#include "builder/ast/calls/SolTypeConversion.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"

#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/eb/TypeConversions.h"
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
		&& !m_call.arguments().empty())
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(
				m_call.arguments()[0].get());
			ident && ident->name() == "this")
			return awst::makeGlobal(
				std::string("CurrentApplicationAddress"), awst::WType::accountType(), m_loc);

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
	if (auto converted = eb::TypeConversions::tryConvert(
		m_ctx, solTarget, targetType, argument, m_loc))
		return converted->resolve();
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
	unsigned numMembers = enumType->numberOfMembers();

	// Range-check the FULL value BEFORE truncating to uint64. A wide input
	// (int136 etc. = biguint) truncated first would drop its high bits, so a
	// value whose full magnitude is out of range but whose LOW 64 bits form a
	// valid ordinal (e.g. int136 -2^135 → low64 == 0) slipped the check and
	// returned the WRONG enum member instead of Panic(0x21). The constant is
	// typed to the value's width so a biguint value compares at biguint width
	// (a canonical negative = 2^256-k is > numMembers → reverts). Found by the
	// corpus-mutation fuzzer (internal_library_function_attached_to_enum
	// uint256->int136).
	auto argOnce = awst::makeEvalOnce(std::move(argExpr), m_loc);
	auto numConst = awst::makeIntegerConstant(numMembers, m_loc, argOnce->wtype);
	m_ctx.preEffects().push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(argOnce, awst::NumericComparison::Lt,
				std::move(numConst), m_loc),
			m_loc, "enum out of range"),
		m_loc));

	return TypeCoercion::implicitNumericCast(argOnce, awst::WType::uint64Type(), m_loc);
}

} // namespace puyasol::builder::sol_ast
