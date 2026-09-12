/// @file TypeConversions.cpp
/// Solidity type conversion handlers.

#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-types/TypeCoercion.h"

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
		auto promoted = TypeCoercion::implicitNumericCast(
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

	auto const* source = _arg->wtype;
	int const width = static_cast<int>(fbType->numBytes());
	if (source == _targetWType)
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(_arg));
	std::shared_ptr<awst::Expression> value;
	if (source == awst::WType::uint64Type() || source == awst::WType::biguintType()
		|| source == awst::WType::accountType())
	{
		// Integer/address magnitudes are right-aligned; signed carriers already
		// contain two's-complement bits. Keep exactly solc's declared byte width.
		if (source == awst::WType::uint64Type()) value = awst::makeItob(std::move(_arg), _loc);
		else value = awst::makeAsBytes(std::move(_arg), _loc);
		value = awst::makeLeftPadToN(std::move(value), width, _loc);
	}
	else if (auto literal = TypeCoercion::stringToBytesN(_arg.get(), _targetWType, width, _loc))
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(literal));
	else if (source && source->kind() == awst::WTypeKind::Bytes)
	{
		// Fixed/dynamic byte strings are left-aligned: extend on the right,
		// then select their prefix. Known lengths avoid unnecessary padding.
		auto const length = awst::fixedBytesLength(source);
		value = awst::makeAsBytes(std::move(_arg), _loc);
		if (!length || *length < width)
			value = awst::makeRightPad(std::move(value), width - length.value_or(0), _loc);
		if (!length || *length != width)
			value = awst::makeExtract(std::move(value), 0, width, _loc);
	}
	else return nullptr;
	return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType,
		awst::makeReinterpretCast(std::move(value), _targetWType, _loc));
}

// ─────────────────────────────────────────────────────────────────────
} // namespace puyasol::builder::eb
