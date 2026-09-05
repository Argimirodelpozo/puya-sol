/// @file TypeConversions.cpp
/// Solidity type conversion handlers.

#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::eb
{

TypeConversionRegistry::TypeConversionRegistry()
{
	registerHandler(solidity::frontend::Type::Category::Bool, &convertToBool);
	registerHandler(solidity::frontend::Type::Category::Address, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::Contract, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::FixedBytes, &convertToFixedBytes);
}

void TypeConversionRegistry::registerHandler(
	solidity::frontend::Type::Category _cat, ConvertHandler _handler)
{
	m_handlers[static_cast<int>(_cat)] = std::move(_handler);
}

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::tryConvert(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc) const
{
	if (!_targetSolType) return nullptr;
	auto it = m_handlers.find(static_cast<int>(_targetSolType->category()));
	if (it != m_handlers.end())
		return it->second(_ctx, _targetSolType, _targetWType, std::move(_arg), _loc);
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Bool conversion: bool(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToBool(
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

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToAddress(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto* srcWType = _arg->wtype;

	if (srcWType == awst::WType::accountType())
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
		auto result = awst::makeAsAccount(std::move(_arg), _loc);
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(result));
	}

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// FixedBytes conversion: bytes32(x), bytes4(x), etc.
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToFixedBytes(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(_targetSolType);
	if (!fbType) return nullptr;

	auto* srcWType = _arg->wtype;

	// Same type → no-op
	if (srcWType == _targetWType)
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(_arg));

	if (srcWType == awst::WType::uint64Type())
	{
		unsigned byteWidth = fbType->numBytes();
		auto itob = awst::makeItob(std::move(_arg), _loc);

		std::shared_ptr<awst::Expression> result;
		if (byteWidth < 8)
		{
			auto off = awst::makeIntegerConstant(8 - byteWidth, _loc);
			auto len = awst::makeIntegerConstant(byteWidth, _loc);

			auto extract = awst::makeExtract3(std::move(itob), std::move(off), std::move(len), _loc);
			result = std::move(extract);
		}
		else if (byteWidth > 8)
		{
			result = awst::makeLeftPadToN(std::move(itob), byteWidth, _loc);
		}
		else
			result = std::move(itob);

		auto cast = awst::makeReinterpretCast(std::move(result), _targetWType, _loc);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	if (srcWType == awst::WType::biguintType())
	{
		unsigned byteWidth = fbType->numBytes();
		auto toBytes = awst::makeAsBytes(std::move(_arg), _loc);

		auto padded = awst::makeLeftPadToN(std::move(toBytes), byteWidth, _loc);

		auto cast = awst::makeReinterpretCast(std::move(padded), _targetWType, _loc);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	// FixedBytes[M]→FixedBytes[N]: right-pad or left-truncate.
	if (srcWType && srcWType->kind() == awst::WTypeKind::Bytes)
	{
		auto const* srcBytes = dynamic_cast<awst::BytesWType const*>(srcWType);
		int srcLen = srcBytes && srcBytes->length() ? *srcBytes->length() : 0;
		int tgtLen = static_cast<int>(fbType->numBytes());

		// UNSIZED `bytes memory` → bytesN. Solidity takes the FIRST N bytes,
		// right-padding with zeros: the value is LEFT-aligned in the word. This
		// fell through to a bare reinterpret, so a later `uint256(...)` read the
		// short byte string as a NUMBER and right-aligned it — `bytes32("abc")`
		// came out 0x00…616263 instead of 0x616263…00. OZ ShortStrings packs
		// `bytes32(uint256(bytes32(bstr)) | bstr.length)`, so the length was
		// OR'd onto the last DATA byte instead of the length byte
		// ("hello world" round-tripped as "hello worlo") and byteLength()
		// returned a character code. Blocked usde/kaito/ena/aero/velo.
		if (srcLen == 0 && tgtLen > 0)
		{
			auto toBytes = awst::makeAsBytes(std::move(_arg), _loc);
			auto padded = awst::makeConcat(
				std::move(toBytes), awst::makeBzero(tgtLen, _loc), _loc);
			auto result = awst::makeExtract(std::move(padded), 0, tgtLen, _loc);
			auto cast = awst::makeReinterpretCast(std::move(result), _targetWType, _loc);
			return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
		}

		if (srcLen > 0 && tgtLen > 0 && srcLen != tgtLen)
		{
			auto toBytes = awst::makeAsBytes(std::move(_arg), _loc);
			std::shared_ptr<awst::Expression> result;
			if (tgtLen > srcLen)
				result = awst::makeRightPad(std::move(toBytes), tgtLen - srcLen, _loc);
			else
				result = awst::makeExtract(std::move(toBytes), 0, tgtLen, _loc);
			auto cast = awst::makeReinterpretCast(std::move(result), _targetWType, _loc);
			return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
		}

		auto cast = awst::makeReinterpretCast(std::move(_arg), _targetWType, _loc);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
} // namespace puyasol::builder::eb
