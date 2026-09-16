/// @file TypeConversions.cpp
/// Solidity type conversion handlers.

#include "builder/eb/TypeConversions.h"
#include "builder/eb/SolAddressBuilder.h"
#include "builder/eb/SolBoolBuilder.h"
#include "builder/eb/SolFixedBytesBuilder.h"
#include "builder/types/TypeCoercion.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> TypeConversions::tryConvert(
	ContractContext& ctx, solidity::frontend::Type const* targetSol,
	awst::WType const* target, std::shared_ptr<awst::Expression> arg,
	awst::SourceLocation const& loc)
{
	using Category = solidity::frontend::Type::Category;
	switch (targetSol->category())
	{
	case Category::Bool: return convertToBool(ctx, targetSol, target, std::move(arg), loc);
	case Category::Address:
	case Category::Contract: return convertToAddress(ctx, targetSol, target, std::move(arg), loc);
	case Category::FixedBytes: return convertToFixedBytes(ctx, targetSol, target, std::move(arg), loc);
	default: return nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────
// Bool conversion: bool(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversions::convertToBool(
	ContractContext& _ctx,
	solidity::frontend::Type const* /*_targetSolType*/,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	if (awst::isNumericWType(_arg->wtype))
	{
		auto zero = awst::makeZero(_loc, _arg->wtype);

		auto cmp = awst::makeNumericCompare(std::move(_arg), awst::NumericComparison::Ne, std::move(zero), _loc);

		return std::make_unique<SolBoolBuilder>(_ctx, std::move(cmp));
	}

	if (_arg->wtype == awst::WType::boolType())
		return std::make_unique<SolBoolBuilder>(_ctx, std::move(_arg));

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Address conversion: address(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversions::convertToAddress(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto* srcWType = _arg->wtype;

	if (srcWType == awst::WType::applicationType())
		_arg = TypeCoercion::coerceForAssignment(std::move(_arg), awst::WType::accountType(), _loc);

	if (_arg->wtype == awst::WType::accountType())
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(_arg));

	// Integer → left-pad to 32 bytes → account.
	if (awst::isNumericWType(srcWType))
	{
		auto promoted = TypeCoercion::coerceScalar(
			std::move(_arg), awst::WType::biguintType(), _loc);
		auto toBytes = awst::makeAsBytes(std::move(promoted), _loc);

		auto padded = awst::makeLeftPadToN(std::move(toBytes), 32, _loc);

		auto result = awst::makeAsAccount(std::move(padded), _loc);
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(result));
	}

	if (srcWType == awst::WType::bytesType()
		|| (srcWType && srcWType->kind() == awst::WTypeKind::Bytes))
	{
		auto result = awst::makeAsAccount(awst::makeLeftPadToN(
			awst::makeAsBytes(std::move(_arg), _loc), 32, _loc), _loc);
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(result));
	}

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// FixedBytes conversion: bytes32(x), bytes4(x), etc.
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversions::convertToFixedBytes(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(_targetSolType);
	if (!fbType) return nullptr;

	return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType,
		TypeCoercion::coerceScalar(std::move(_arg), _targetWType, _loc));
}

// ─────────────────────────────────────────────────────────────────────
} // namespace puyasol::builder::eb
