/// @file TypeConversions.cpp
/// Solidity type conversion handlers.

#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolEnumBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::eb
{

/// Minimal InstanceBuilder for conversion results.
class GenericConvertBuilder: public InstanceBuilder
{
public:
	GenericConvertBuilder(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr))
	{
	}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

TypeConversionRegistry::TypeConversionRegistry()
{
	registerHandler(solidity::frontend::Type::Category::Integer, &convertToInteger);
	registerHandler(solidity::frontend::Type::Category::Bool, &convertToBool);
	registerHandler(solidity::frontend::Type::Category::Address, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::Contract, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::FixedBytes, &convertToFixedBytes);
	registerHandler(solidity::frontend::Type::Category::Enum, &convertToEnum);
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
// Integer conversion: uint8(x), uint256(x), int64(x), etc.
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToInteger(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* targetInt = dynamic_cast<solidity::frontend::IntegerType const*>(_targetSolType);
	if (!targetInt) return nullptr;

	unsigned targetBits = targetInt->numBits();
	bool targetIsBigUInt = targetBits > 64;
	auto* srcWType = _arg->wtype;

	if (srcWType == _targetWType)
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(_arg));

	if (!targetIsBigUInt && srcWType == awst::WType::biguintType())
	{
		// biguint→uint64: safe extraction (btoi fails on >8 bytes).
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), awst::WType::uint64Type(), _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	if (targetIsBigUInt && srcWType == awst::WType::uint64Type())
	{
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	if (targetIsBigUInt && srcWType == awst::WType::biguintType())
	{
		if (targetBits < 256)
		{
			solidity::u256 mask = (solidity::u256(1) << targetBits) - 1;
			auto maskConst = awst::makeIntegerConstant(mask.str(), _loc, awst::WType::biguintType());

			auto masked = awst::makeBigUIntBinOp(std::move(_arg), awst::BigUIntBinaryOperator::BitAnd, std::move(maskConst), _loc);

			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(masked));
		}
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(_arg));
	}

	if (srcWType == awst::WType::boolType())
	{
		// bool is 0/1 on AVM; just cast width.
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	// account→integer: AVM addresses are 32-byte keys; reinterpret as biguint,
	// then narrow or extract to uint64 as needed.
	if (srcWType == awst::WType::accountType())
	{
		auto asBiguint = awst::makeAsBiguint(std::move(_arg), _loc);
		// Apply target-bit-width masking if narrower than 256 bits.
		if (targetIsBigUInt && targetBits < 256)
		{
			solidity::u256 mask = (solidity::u256(1) << targetBits) - 1;
			auto maskConst = awst::makeIntegerConstant(
				mask.str(), _loc, awst::WType::biguintType());
			auto masked = awst::makeBigUIntBinOp(
				std::move(asBiguint), awst::BigUIntBinaryOperator::BitAnd,
				std::move(maskConst), _loc);
			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(masked));
		}
		if (!targetIsBigUInt)
		{
			auto narrowed = TypeCoercion::implicitNumericCast(
				std::move(asBiguint), awst::WType::uint64Type(), _loc);
			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(narrowed));
		}
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(asBiguint));
	}

	if (srcWType && srcWType->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(srcWType);
		// Unsized or >8-byte (e.g. keccak256 32-byte digest) → biguint; btoi only handles ≤8.
		bool knownSmall =
			bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
		if (!knownSmall)
		{
			auto cast = awst::makeAsBiguint(std::move(_arg), _loc);
			auto result = TypeCoercion::implicitNumericCast(std::move(cast), _targetWType, _loc);
			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
		}
		auto toBytes = awst::makeAsBytes(std::move(_arg), _loc);
		auto btoi = awst::makeBtoi(std::move(toBytes), _loc);
		auto result = TypeCoercion::implicitNumericCast(std::move(btoi), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
	return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
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
// Enum conversion: MyEnum(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToEnum(
	ContractContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(_targetSolType);
	if (!enumType) return nullptr;

	// Range-check the FULL value BEFORE truncating to uint64 (see the parallel
	// SolTypeConversion::handleEnumConversion): a wide biguint input truncated
	// first drops its high bits, so a value out of range whose low 64 bits form a
	// valid ordinal returned the wrong member instead of Panic(0x21). Typed
	// constant so a biguint value compares at biguint width.
	unsigned numMembers = enumType->numberOfMembers();
	auto argOnce = awst::makeEvalOnce(std::move(_arg), _loc);
	auto numConst = awst::makeIntegerConstant(numMembers, _loc, argOnce->wtype);
	_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(argOnce, awst::NumericComparison::Lt,
				std::move(numConst), _loc),
			_loc, "enum out of range"),
		_loc));

	auto result = TypeCoercion::implicitNumericCast(argOnce, awst::WType::uint64Type(), _loc);
	return std::make_unique<SolEnumBuilder>(_ctx, enumType, std::move(result));
}

} // namespace puyasol::builder::eb
