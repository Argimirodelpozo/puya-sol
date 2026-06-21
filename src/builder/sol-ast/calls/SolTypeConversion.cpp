/// @file SolTypeConversion.cpp
/// Type conversion calls: uint256(x), address(y), bytes32(z), bool(w), etc.
/// Migrated from FunctionCallBuilder.cpp lines 117-784.

#include "builder/sol-ast/calls/SolTypeConversion.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

SolTypeConversion::SolTypeConversion(
	eb::ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolFunctionCall(_ctx, _call)
{
}

std::shared_ptr<awst::Expression> SolTypeConversion::toAwst()
{
	if (m_call.arguments().empty())
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	// Enum range check: EnumType(x) must assert x < numMembers
	if (dynamic_cast<solidity::frontend::EnumType const*>(m_call.annotation().type))
		return handleEnumConversion();

	// Try TypeConversionRegistry first (handles integer, bool, address, fixedbytes, enum)
	{
		auto argExpr = buildExpr(*m_call.arguments()[0]);
		eb::TypeConversionRegistry registry;
		auto converted = registry.tryConvert(
			m_ctx, m_call.annotation().type, targetType,
			argExpr, m_loc);
		if (converted)
		{
			auto result = converted->resolve();
			// Narrowing mask: Solidity truncates to target width regardless of sign.
			// Sign-extension on subsequent widening reads from the source-width MSB.
			result = applyNarrowingMask(std::move(result), targetType);

			// Signed sub-word widen (e.g. int128(someInt24)): registry uses
			// zero-extension (drops sign: -60 int24 → +16777156). Re-extend from
			// SOURCE width via signExtendToUint256 (masks to N bits first; safe
			// whether uint64 is raw sub-word or already sign-extended).
			if (auto const* srcInt = dynamic_cast<solidity::frontend::IntegerType const*>(
					m_call.arguments()[0]->annotation().type))
			{
				auto const* tgtInt = dynamic_cast<solidity::frontend::IntegerType const*>(
					m_call.annotation().type);
				if (tgtInt && result->wtype == awst::WType::biguintType())
				{
					if (srcInt->isSigned()
						&& srcInt->numBits() <= 64 && tgtInt->numBits() > 64)
					{
						result = TypeCoercion::signExtendToUint256(
							std::move(result), srcInt->numBits(), m_loc);
					}
					else if (tgtInt->isSigned() && srcInt->numBits() > 64
						&& tgtInt->numBits() > 64 && tgtInt->numBits() < 256)
					{
						// biguint→signed intN (e.g. `int128(int256)`, SafeCast.toIntN):
						// mask already gave low N bits; sign-extend to 256-bit two's-complement
						// so negatives round-trip and biguint widen is re-canonicalised.
						result = TypeCoercion::signExtendToUint256(
							std::move(result), tgtInt->numBits(), m_loc);
					}
				}
				else if (tgtInt && result->wtype == awst::WType::uint64Type()
					&& srcInt->isSigned() && tgtInt->isSigned()
					&& srcInt->numBits() < tgtInt->numBits())
				{
					// Signed sub-word -> WIDER sub-word (both <=64-bit, uint64-backed): the registry
					// zero-extends (drops the sign, e.g. int16(int8(-1)) -> 255). Re-extend from the
					// SOURCE width so the value stays signed-correct.
					result = TypeCoercion::signExtendToUint64(
						std::move(result), srcInt->numBits(), m_loc);
				}
			}

			// Final canonicalisation: any signed sub-word (<64-bit, uint64-backed)
			// target must be sign-extended from its OWN width, so the low-N-bits
			// narrowing mask becomes a canonical 64-bit two's-complement. Covers
			// constant/Rational sources (srcInt is null above, e.g. int8(-1) -> 0xff,
			// which then mis-compared `== int8(-1)` and broke raw-value consumers)
			// and narrowing casts the source-width branch skips. signExtendToUint64
			// masks first, so it is idempotent when the value is already canonical.
			if (auto const* tgtInt = dynamic_cast<solidity::frontend::IntegerType const*>(
					m_call.annotation().type))
			{
				if (tgtInt->isSigned() && tgtInt->numBits() < 64
					&& result->wtype == awst::WType::uint64Type())
					result = TypeCoercion::signExtendToUint64(
						std::move(result), tgtInt->numBits(), m_loc);
			}
			return result;
		}
	}

	// address(0) special constant
	if (targetType == awst::WType::accountType())
	{
		auto addrZero = tryAddressZeroConstant();
		if (addrZero) return addrZero;

		// address(integer) / address(bytes) — TypeConversion registry should handle these,
		// but if not, fall through to generic conversion below.
		auto argExpr = buildExpr(*m_call.arguments()[0]);

		// address(application) → app address via app_params_get AppAddress
		if (argExpr->wtype == awst::WType::applicationType())
		{
			return TypeCoercion::coerceForAssignment(
				std::move(argExpr), awst::WType::accountType(), m_loc);
		}

		if (argExpr->wtype == awst::WType::uint64Type()
			|| argExpr->wtype == awst::WType::biguintType())
		{
			auto promoted = TypeCoercion::implicitNumericCast(
				std::move(argExpr), awst::WType::biguintType(), m_loc);
			auto toBytes = awst::makeAsBytes(std::move(promoted), m_loc);

			auto padded = awst::makeLeftPadToN(std::move(toBytes), 32, m_loc);
			auto addrCast = awst::makeAsAccount(std::move(padded), m_loc);
			return addrCast;
		}

		// bytes → account
		auto addrCast = awst::makeAsAccount(std::move(argExpr), m_loc);
		return addrCast;
	}

	return handleGenericConversion(targetType);
}

// ─────────────────────────────────────────────────────────────────────
// Enum conversion with range check
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::handleEnumConversion()
{
	auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(
		m_call.annotation().type);
	auto argExpr = buildExpr(*m_call.arguments()[0]);
	auto result = TypeCoercion::implicitNumericCast(
		std::move(argExpr), awst::WType::uint64Type(), m_loc);

	unsigned numMembers = enumType->numberOfMembers();
	auto stmt = awst::makeExpressionStatement(
		awst::makeEnumRangeAssert(result, numMembers, m_loc), m_loc);
	m_ctx.prePendingStatements.push_back(std::move(stmt));

	return result;
}

// ─────────────────────────────────────────────────────────────────────
// address(0) → zero address constant
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::tryAddressZeroConstant()
{
	auto const& arg = *m_call.arguments()[0];
	if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(&arg))
	{
		if (lit->value() == "0")
			return awst::makeAddressConstant(
				"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ", m_loc);
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Generic conversion: narrowing casts, bytes conversions, etc.
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::handleGenericConversion(
	awst::WType const* _targetType)
{
	auto converted = buildExpr(*m_call.arguments()[0]);
	converted = TypeCoercion::implicitNumericCast(std::move(converted), _targetType, m_loc);

	// Apply narrowing mask for integer casts
	converted = applyNarrowingMask(std::move(converted), _targetType);

	if (_targetType == converted->wtype)
		return converted;

	bool sourceIsBytes = converted->wtype && converted->wtype->kind() == awst::WTypeKind::Bytes;
	bool targetIsUint = _targetType == awst::WType::uint64Type();
	bool targetIsBiguint = _targetType == awst::WType::biguintType();
	bool sourceIsUint = converted->wtype == awst::WType::uint64Type();
	bool sourceIsBiguint = converted->wtype == awst::WType::biguintType();
	bool targetIsBytes = _targetType && _targetType->kind() == awst::WTypeKind::Bytes;

	// bytes[N] → uint64
	if (sourceIsBytes && targetIsUint)
	{
		auto expr = std::move(converted);
		if (expr->wtype != awst::WType::bytesType())
		{
			auto toBytes = awst::makeAsBytes(std::move(expr), m_loc);
			expr = std::move(toBytes);
		}
		auto btoi = awst::makeBtoi(std::move(expr), m_loc);
		std::shared_ptr<awst::Expression> result = std::move(btoi);

		// Narrowing mask for bytes→uint16 etc.
		auto const* solTargetType = m_call.annotation().type;
		if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
		{
			unsigned targetBits = intType->numBits();
			if (targetBits < 64)
			{
				auto mask = awst::makeIntegerConstant((uint64_t(1) << targetBits) - 1, m_loc);

				auto bitAnd = awst::makeUInt64BinOp(std::move(result), awst::UInt64BinaryOperator::BitAnd, std::move(mask), m_loc);
				result = std::move(bitAnd);
			}
		}
		return result;
	}

	// bytes[N] → biguint
	if (sourceIsBytes && targetIsBiguint)
	{
		auto expr = std::move(converted);
		if (expr->wtype != awst::WType::bytesType())
		{
			auto toBytes = awst::makeAsBytes(std::move(expr), m_loc);
			expr = std::move(toBytes);
		}
		auto cast = awst::makeAsBiguint(std::move(expr), m_loc);
		return cast;
	}

	// uint64 → bytes[N]
	if (sourceIsUint && targetIsBytes)
	{
		int byteWidth = 8;
		auto const* solTargetType = m_call.annotation().type;
		if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
			byteWidth = static_cast<int>(fbType->numBytes());
		return handleIntToBytes(std::move(converted), byteWidth);
	}

	// biguint → bytes[N]
	if (sourceIsBiguint && targetIsBytes)
	{
		int byteWidth = 32;
		auto const* solTargetType = m_call.annotation().type;
		if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
			byteWidth = static_cast<int>(fbType->numBytes());
		return handleBiguintToBytes(std::move(converted), byteWidth);
	}

	// bytes → fixed-size array decomposition
	if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_targetType))
	{
		auto arrSize = refArr->arraySize();
		if (arrSize && *arrSize > 0)
		{
			auto* elemType = refArr->elementType();
			int elemSize = (elemType == awst::WType::uint64Type()) ? 8 : 32;

			auto bytesSource = std::move(converted);
			if (bytesSource->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeAsBytes(std::move(bytesSource), m_loc);
				bytesSource = std::move(toBytes);
			}

			auto arr = awst::makeNewArray(_targetType, m_loc);
			for (int i = 0; i < *arrSize; ++i)
			{
				auto off = awst::makeIntegerConstant(i * elemSize, m_loc);
				auto len = awst::makeIntegerConstant(elemSize, m_loc);
				auto extract = awst::makeExtract3(
					bytesSource, std::move(off), std::move(len), m_loc);

				if (elemType == awst::WType::biguintType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), elemType, m_loc);
					arr->values.push_back(std::move(cast));
				}
				else if (elemType == awst::WType::uint64Type())
				{
					arr->values.push_back(
						awst::makeBtoi(std::move(extract), m_loc));
				}
				else
					arr->values.push_back(std::move(extract));
			}
			return arr;
		}
		auto arr = awst::makeNewArray(_targetType, m_loc);
		return arr;
	}

	// bytes[M] → bytes[N]: pad or truncate. Also handles dynamic `bytes`
	// → bytes[N] (sourceWidth == 0 means runtime length); we need to
	// extract3 the first N bytes at runtime instead of falling through
	// to a plain ReinterpretCast (which produces a length mismatch in
	// puya for fixed bytes targets).
	if (sourceIsBytes && targetIsBytes)
	{
		int sourceWidth = 0, targetWidth = 0;
		if (auto const* sw = dynamic_cast<awst::BytesWType const*>(converted->wtype))
			sourceWidth = sw->length() ? *sw->length() : 0;
		auto const* solTargetType = m_call.annotation().type;
		if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
			targetWidth = static_cast<int>(fbType->numBytes());
		if (!targetWidth)
			if (auto const* tw = dynamic_cast<awst::BytesWType const*>(_targetType))
				targetWidth = tw->length() ? *tw->length() : 0;

		// Dynamic bytes → bytes[N]: right-pad shorter, truncate longer.
		// concat(src, bzero(N)) ensures extract3(0,N) never goes out of bounds.
		if (targetWidth > 0 && sourceWidth == 0)
		{
			auto srcBytes = std::move(converted);
			if (srcBytes->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeAsBytes(std::move(srcBytes), m_loc);
				srcBytes = std::move(toBytes);
			}

			auto cat = awst::makeRightPad(std::move(srcBytes), targetWidth, m_loc);
			auto extract = awst::makeExtract(std::move(cat), 0, targetWidth, m_loc);
			return awst::makeReinterpretCast(std::move(extract), _targetType, m_loc);
		}

		if (targetWidth > 0 && sourceWidth > 0 && targetWidth != sourceWidth)
		{
			auto expr = std::move(converted);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeAsBytes(std::move(expr), m_loc);
				expr = std::move(toBytes);
			}

			std::shared_ptr<awst::Expression> result;
			if (targetWidth > sourceWidth)
			{
				// Right-pad: concat(input, bzero(N-M))
				result = awst::makeRightPad(std::move(expr), targetWidth - sourceWidth, m_loc);
			}
			else
			{
				// Truncate: extract3(input, 0, N)
				result = awst::makeExtract(std::move(expr), 0, targetWidth, m_loc);
			}

			auto finalCast = awst::makeReinterpretCast(std::move(result), _targetType, m_loc);
			return finalCast;
		}
	}

	// Default: ReinterpretCast
	auto cast = awst::makeReinterpretCast(std::move(converted), _targetType, m_loc);
	return cast;
}

// ─────────────────────────────────────────────────────────────────────
// Narrowing mask helper
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::applyNarrowingMask(
	std::shared_ptr<awst::Expression> _expr, awst::WType const* _targetType)
{
	auto const* solTargetType = m_call.annotation().type;
	auto const* solSourceType = m_call.arguments()[0]->annotation().type;
	auto const* targetIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType);
	if (!targetIntType) return _expr;

	unsigned targetBits = targetIntType->numBits();

	// uint64 narrowing: also covers signed→unsigned at same width
	// (e.g. int8→uint8: uint64(-2)=2^64-2, must mask to 8 bits → 254).
	if (_targetType == awst::WType::uint64Type() && _expr->wtype == awst::WType::uint64Type())
	{
		unsigned sourceBits = 64;
		bool sourceIsSigned = false;
		if (auto const* srcInt = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
		{
			sourceBits = srcInt->numBits();
			sourceIsSigned = srcInt->isSigned();
		}
		if (targetBits < 64 && (targetBits < sourceBits || (sourceIsSigned && !targetIntType->isSigned())))
		{
			auto mask = awst::makeIntegerConstant((uint64_t(1) << targetBits) - 1, m_loc);
			auto bitAnd = awst::makeUInt64BinOp(std::move(_expr), awst::UInt64BinaryOperator::BitAnd, std::move(mask), m_loc);
			return bitAnd;
		}
	}

	// biguint narrowing
	if (_targetType == awst::WType::biguintType() && _expr->wtype == awst::WType::biguintType())
	{
		unsigned sourceBits = 256;
		if (auto const* srcInt = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
			sourceBits = srcInt->numBits();
		if (targetBits < sourceBits && targetBits < 256)
		{
			// Use `b%` not `b&`: AVM `b&` returns max(len(a),len(b)) bytes
			// (no leading-zero strip) — 32-byte ARC4 value & 16-byte mask
			// leaves 32 bytes → fails `to_fixed_size` len check. `b%` gives
			// minimum-length result. `b+ x 0` is folded by puya; `b%` is not.
			solidity::u256 modulusVal = solidity::u256(1) << targetBits;
			auto modulus = awst::makeIntegerConstant(modulusVal.str(), m_loc, awst::WType::biguintType());
			auto bMod = awst::makeBigUIntBinOp(std::move(_expr), awst::BigUIntBinaryOperator::Mod, std::move(modulus), m_loc);
			return bMod;
		}
	}

	return _expr;
}

// ─────────────────────────────────────────────────────────────────────
// Integer → bytes[N]
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::handleIntToBytes(
	std::shared_ptr<awst::Expression> _expr, int _byteWidth)
{
	std::shared_ptr<awst::Expression> result = awst::makeItob(std::move(_expr), m_loc);
	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (_byteWidth < 8)
	{
		result = extractLastN(std::move(result), _byteWidth);
	}
	else if (_byteWidth > 8)
	{
		result = awst::makeLeftPadToN(std::move(result), _byteWidth, m_loc);
	}

	if (targetType != awst::WType::bytesType())
	{
		auto cast = awst::makeReinterpretCast(std::move(result), targetType, m_loc);
		return cast;
	}
	return result;
}

// ─────────────────────────────────────────────────────────────────────
// Biguint → bytes[N]
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::handleBiguintToBytes(
	std::shared_ptr<awst::Expression> _expr, int _byteWidth)
{
	auto toBytes = awst::makeAsBytes(std::move(_expr), m_loc);

	auto result = awst::makeLeftPadToN(std::move(toBytes), _byteWidth, m_loc);
	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (targetType != awst::WType::bytesType())
	{
		auto cast = awst::makeReinterpretCast(std::move(result), targetType, m_loc);
		return cast;
	}
	return result;
}

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolTypeConversion::extractLastN(
	std::shared_ptr<awst::Expression> _expr, int _n)
{
	auto offsetConst = awst::makeIntegerConstant(8 - _n, m_loc);

	auto widthConst = awst::makeIntegerConstant(_n, m_loc);

	auto extract = awst::makeExtract3(std::move(_expr), std::move(offsetConst), std::move(widthConst), m_loc);
	return extract;
}

} // namespace puyasol::builder::sol_ast
