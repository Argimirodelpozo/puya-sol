/// @file SolTypeConversion.cpp
/// Type conversion calls: uint256(x), address(y), bytes32(z), bool(w), etc.
/// Migrated from FunctionCallBuilder.cpp lines 117-784.

#include "builder/sol-ast/calls/SolTypeConversion.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

SolTypeConversion::SolTypeConversion(
	eb::BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolFunctionCall(_ctx, _call)
{
}

std::shared_ptr<awst::Expression> SolTypeConversion::toAwst()
{
	if (m_call.arguments().empty())
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	// Enum range check: EnumType(x) must assert x < numMembers
	if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(
			m_call.annotation().type))
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
			// Apply narrowing mask for unsigned types when registry returns as-is.
			// uint32→uint16 (both uint64) needs truncation to target width.
			// Skip for signed types — masking strips sign extension.
			auto const* targetIntType = dynamic_cast<solidity::frontend::IntegerType const*>(
				m_call.annotation().type);
			if (!targetIntType || !targetIntType->isSigned())
				result = applyNarrowingMask(std::move(result), targetType);
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
			auto toBytes = awst::makeReinterpretCast(std::move(promoted), awst::WType::bytesType(), m_loc);

			auto padded = leftPadToN(std::move(toBytes), 32);
			auto addrCast = awst::makeReinterpretCast(std::move(padded), awst::WType::accountType(), m_loc);
			return addrCast;
		}

		// bytes → account
		auto addrCast = awst::makeReinterpretCast(std::move(argExpr), awst::WType::accountType(), m_loc);
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
	auto maxVal = awst::makeIntegerConstant(std::to_string(numMembers), m_loc);

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = m_loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = result;
	cmp->op = awst::NumericComparison::Lt;
	cmp->rhs = std::move(maxVal);

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = m_loc;
	stmt->expr = awst::makeAssert(std::move(cmp), m_loc, "enum out of range");
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
		{
			auto e = std::make_shared<awst::AddressConstant>();
			e->sourceLocation = m_loc;
			e->wtype = awst::WType::accountType();
			e->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
			return e;
		}
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
			auto toBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), m_loc);
			expr = std::move(toBytes);
		}
		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = m_loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(expr));

		std::shared_ptr<awst::Expression> result = std::move(btoi);

		// Narrowing mask for bytes→uint16 etc.
		auto const* solTargetType = m_call.annotation().type;
		if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
		{
			unsigned targetBits = intType->numBits();
			if (targetBits < 64)
			{
				auto mask = awst::makeIntegerConstant(std::to_string((uint64_t(1) << targetBits) - 1), m_loc);

				auto bitAnd = std::make_shared<awst::UInt64BinaryOperation>();
				bitAnd->sourceLocation = m_loc;
				bitAnd->wtype = awst::WType::uint64Type();
				bitAnd->left = std::move(result);
				bitAnd->op = awst::UInt64BinaryOperator::BitAnd;
				bitAnd->right = std::move(mask);
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
			auto toBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), m_loc);
			expr = std::move(toBytes);
		}
		auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::biguintType(), m_loc);
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
				auto toBytes = awst::makeReinterpretCast(std::move(bytesSource), awst::WType::bytesType(), m_loc);
				bytesSource = std::move(toBytes);
			}

			auto arr = std::make_shared<awst::NewArray>();
			arr->sourceLocation = m_loc;
			arr->wtype = _targetType;
			for (int i = 0; i < *arrSize; ++i)
			{
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(bytesSource);
				auto off = awst::makeIntegerConstant(std::to_string(i * elemSize), m_loc);
				extract->stackArgs.push_back(std::move(off));
				auto len = awst::makeIntegerConstant(std::to_string(elemSize), m_loc);
				extract->stackArgs.push_back(std::move(len));

				if (elemType == awst::WType::biguintType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), elemType, m_loc);
					arr->values.push_back(std::move(cast));
				}
				else if (elemType == awst::WType::uint64Type())
				{
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = m_loc;
					btoi->wtype = awst::WType::uint64Type();
					btoi->opCode = "btoi";
					btoi->stackArgs.push_back(std::move(extract));
					arr->values.push_back(std::move(btoi));
				}
				else
					arr->values.push_back(std::move(extract));
			}
			return arr;
		}
		auto arr = std::make_shared<awst::NewArray>();
		arr->sourceLocation = m_loc;
		arr->wtype = _targetType;
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

		// Dynamic bytes → fixed bytes[N]: Solidity right-pads if the source
		// has fewer than N bytes and truncates otherwise. Build:
		//   let m = min(len(src), N)
		//   extract3(concat(src, bzero(N)), 0, N)
		// `concat(src, bzero(N))` guarantees there is always at least N
		// bytes available so the extract3 doesn't go out of bounds when
		// the input is shorter than the target width. The leading N bytes
		// of the result are exactly the source's first N bytes (zero-padded
		// on the right if the source was shorter), matching Solidity's
		// `bytesN(bytes_dynamic)` semantics.
		if (targetWidth > 0 && sourceWidth == 0)
		{
			auto srcBytes = std::move(converted);
			if (srcBytes->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeReinterpretCast(std::move(srcBytes), awst::WType::bytesType(), m_loc);
				srcBytes = std::move(toBytes);
			}

			auto padSize = awst::makeIntegerConstant(std::to_string(targetWidth), m_loc);
			auto pad = std::make_shared<awst::IntrinsicCall>();
			pad->sourceLocation = m_loc;
			pad->wtype = awst::WType::bytesType();
			pad->opCode = "bzero";
			pad->stackArgs.push_back(std::move(padSize));

			auto cat = std::make_shared<awst::IntrinsicCall>();
			cat->sourceLocation = m_loc;
			cat->wtype = awst::WType::bytesType();
			cat->opCode = "concat";
			cat->stackArgs.push_back(std::move(srcBytes));
			cat->stackArgs.push_back(std::move(pad));

			auto zero = awst::makeIntegerConstant("0", m_loc);
			auto width = awst::makeIntegerConstant(std::to_string(targetWidth), m_loc);
			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = m_loc;
			extract->wtype = awst::WType::bytesType();
			extract->opCode = "extract3";
			extract->stackArgs.push_back(std::move(cat));
			extract->stackArgs.push_back(std::move(zero));
			extract->stackArgs.push_back(std::move(width));

			auto finalCast = awst::makeReinterpretCast(std::move(extract), _targetType, m_loc);
			return finalCast;
		}

		if (targetWidth > 0 && sourceWidth > 0 && targetWidth != sourceWidth)
		{
			auto expr = std::move(converted);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), m_loc);
				expr = std::move(toBytes);
			}

			std::shared_ptr<awst::Expression> result;
			if (targetWidth > sourceWidth)
			{
				// Right-pad: concat(input, bzero(N-M))
				auto padSize = awst::makeIntegerConstant(std::to_string(targetWidth - sourceWidth), m_loc);
				auto pad = std::make_shared<awst::IntrinsicCall>();
				pad->sourceLocation = m_loc;
				pad->wtype = awst::WType::bytesType();
				pad->opCode = "bzero";
				pad->stackArgs.push_back(std::move(padSize));
				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = m_loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(expr));
				cat->stackArgs.push_back(std::move(pad));
				result = std::move(cat);
			}
			else
			{
				// Truncate: extract3(input, 0, N)
				auto zero = awst::makeIntegerConstant("0", m_loc);
				auto width = awst::makeIntegerConstant(std::to_string(targetWidth), m_loc);
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(std::move(expr));
				extract->stackArgs.push_back(std::move(zero));
				extract->stackArgs.push_back(std::move(width));
				result = std::move(extract);
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

	// uint64 narrowing — also triggered when casting signed→unsigned at the
	// same width (e.g. int8→uint8): the uint64 representation of -2 is
	// 2^64-2, which must be masked to 8 bits to produce 254.
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
			auto mask = awst::makeIntegerConstant(std::to_string((uint64_t(1) << targetBits) - 1), m_loc);
			auto bitAnd = std::make_shared<awst::UInt64BinaryOperation>();
			bitAnd->sourceLocation = m_loc;
			bitAnd->wtype = awst::WType::uint64Type();
			bitAnd->left = std::move(_expr);
			bitAnd->op = awst::UInt64BinaryOperator::BitAnd;
			bitAnd->right = std::move(mask);
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
			solidity::u256 maskVal = (solidity::u256(1) << targetBits) - 1;
			auto mask = awst::makeIntegerConstant(maskVal.str(), m_loc, awst::WType::biguintType());
			auto bitAnd = std::make_shared<awst::BigUIntBinaryOperation>();
			bitAnd->sourceLocation = m_loc;
			bitAnd->wtype = awst::WType::biguintType();
			bitAnd->left = std::move(_expr);
			bitAnd->op = awst::BigUIntBinaryOperator::BitAnd;
			bitAnd->right = std::move(mask);
			return bitAnd;
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
	auto itob = std::make_shared<awst::IntrinsicCall>();
	itob->sourceLocation = m_loc;
	itob->wtype = awst::WType::bytesType();
	itob->opCode = "itob";
	itob->stackArgs.push_back(std::move(_expr));

	std::shared_ptr<awst::Expression> result = std::move(itob);
	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (_byteWidth < 8)
	{
		result = extractLastN(std::move(result), _byteWidth);
	}
	else if (_byteWidth > 8)
	{
		result = leftPadToN(std::move(result), _byteWidth);
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
	auto toBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), m_loc);

	auto result = leftPadToN(std::move(toBytes), _byteWidth);
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

std::shared_ptr<awst::Expression> SolTypeConversion::leftPadToN(
	std::shared_ptr<awst::Expression> _expr, int _n)
{
	auto nConst = awst::makeIntegerConstant(std::to_string(_n), m_loc);

	auto pad = std::make_shared<awst::IntrinsicCall>();
	pad->sourceLocation = m_loc;
	pad->wtype = awst::WType::bytesType();
	pad->opCode = "bzero";
	pad->stackArgs.push_back(std::move(nConst));

	auto cat = std::make_shared<awst::IntrinsicCall>();
	cat->sourceLocation = m_loc;
	cat->wtype = awst::WType::bytesType();
	cat->opCode = "concat";
	cat->stackArgs.push_back(std::move(pad));
	cat->stackArgs.push_back(std::move(_expr));

	auto lenExpr = std::make_shared<awst::IntrinsicCall>();
	lenExpr->sourceLocation = m_loc;
	lenExpr->wtype = awst::WType::uint64Type();
	lenExpr->opCode = "len";
	lenExpr->stackArgs.push_back(cat);

	auto nConst2 = awst::makeIntegerConstant(std::to_string(_n), m_loc);

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = m_loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(lenExpr);
	offset->right = std::move(nConst2);
	offset->op = awst::UInt64BinaryOperator::Sub;

	auto nConst3 = awst::makeIntegerConstant(std::to_string(_n), m_loc);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = m_loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(cat));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(std::move(nConst3));
	return extract;
}

std::shared_ptr<awst::Expression> SolTypeConversion::extractLastN(
	std::shared_ptr<awst::Expression> _expr, int _n)
{
	auto offsetConst = awst::makeIntegerConstant(std::to_string(8 - _n), m_loc);

	auto widthConst = awst::makeIntegerConstant(std::to_string(_n), m_loc);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = m_loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(_expr));
	extract->stackArgs.push_back(std::move(offsetConst));
	extract->stackArgs.push_back(std::move(widthConst));
	return extract;
}

} // namespace puyasol::builder::sol_ast
