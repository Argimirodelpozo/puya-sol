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

		if (argExpr->wtype == awst::WType::uint64Type()
			|| argExpr->wtype == awst::WType::biguintType())
		{
			auto promoted = TypeCoercion::implicitNumericCast(
				std::move(argExpr), awst::WType::biguintType(), m_loc);
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = m_loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(promoted);

			auto padded = leftPadToN(std::move(toBytes), 32);
			auto addrCast = std::make_shared<awst::ReinterpretCast>();
			addrCast->sourceLocation = m_loc;
			addrCast->wtype = awst::WType::accountType();
			addrCast->expr = std::move(padded);
			return addrCast;
		}

		// bytes → account
		auto addrCast = std::make_shared<awst::ReinterpretCast>();
		addrCast->sourceLocation = m_loc;
		addrCast->wtype = awst::WType::accountType();
		addrCast->expr = std::move(argExpr);
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
	auto maxVal = std::make_shared<awst::IntegerConstant>();
	maxVal->sourceLocation = m_loc;
	maxVal->wtype = awst::WType::uint64Type();
	maxVal->value = std::to_string(numMembers);

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = m_loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = result;
	cmp->op = awst::NumericComparison::Lt;
	cmp->rhs = std::move(maxVal);

	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = m_loc;
	assertExpr->wtype = awst::WType::voidType();
	assertExpr->condition = std::move(cmp);
	assertExpr->errorMessage = "enum out of range";

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = m_loc;
	stmt->expr = std::move(assertExpr);
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
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = m_loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(expr);
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
				auto mask = std::make_shared<awst::IntegerConstant>();
				mask->sourceLocation = m_loc;
				mask->wtype = awst::WType::uint64Type();
				mask->value = std::to_string((uint64_t(1) << targetBits) - 1);

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
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = m_loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(expr);
			expr = std::move(toBytes);
		}
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(expr);
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
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = m_loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(bytesSource);
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
				auto off = std::make_shared<awst::IntegerConstant>();
				off->sourceLocation = m_loc;
				off->wtype = awst::WType::uint64Type();
				off->value = std::to_string(i * elemSize);
				extract->stackArgs.push_back(std::move(off));
				auto len = std::make_shared<awst::IntegerConstant>();
				len->sourceLocation = m_loc;
				len->wtype = awst::WType::uint64Type();
				len->value = std::to_string(elemSize);
				extract->stackArgs.push_back(std::move(len));

				if (elemType == awst::WType::biguintType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = m_loc;
					cast->wtype = elemType;
					cast->expr = std::move(extract);
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

	// bytes[M] → bytes[N]: pad or truncate
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

		if (targetWidth > 0 && sourceWidth > 0 && targetWidth != sourceWidth)
		{
			auto expr = std::move(converted);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = m_loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(expr);
				expr = std::move(toBytes);
			}

			std::shared_ptr<awst::Expression> result;
			if (targetWidth > sourceWidth)
			{
				// Right-pad: concat(input, bzero(N-M))
				auto padSize = std::make_shared<awst::IntegerConstant>();
				padSize->sourceLocation = m_loc;
				padSize->wtype = awst::WType::uint64Type();
				padSize->value = std::to_string(targetWidth - sourceWidth);
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
				auto zero = std::make_shared<awst::IntegerConstant>();
				zero->sourceLocation = m_loc;
				zero->wtype = awst::WType::uint64Type();
				zero->value = "0";
				auto width = std::make_shared<awst::IntegerConstant>();
				width->sourceLocation = m_loc;
				width->wtype = awst::WType::uint64Type();
				width->value = std::to_string(targetWidth);
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(std::move(expr));
				extract->stackArgs.push_back(std::move(zero));
				extract->stackArgs.push_back(std::move(width));
				result = std::move(extract);
			}

			auto finalCast = std::make_shared<awst::ReinterpretCast>();
			finalCast->sourceLocation = m_loc;
			finalCast->wtype = _targetType;
			finalCast->expr = std::move(result);
			return finalCast;
		}
	}

	// Default: ReinterpretCast
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = m_loc;
	cast->wtype = _targetType;
	cast->expr = std::move(converted);
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

	// uint64 narrowing
	if (_targetType == awst::WType::uint64Type() && _expr->wtype == awst::WType::uint64Type())
	{
		unsigned sourceBits = 64;
		if (auto const* srcInt = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
			sourceBits = srcInt->numBits();
		if (targetBits < sourceBits && targetBits < 64)
		{
			auto mask = std::make_shared<awst::IntegerConstant>();
			mask->sourceLocation = m_loc;
			mask->wtype = awst::WType::uint64Type();
			mask->value = std::to_string((uint64_t(1) << targetBits) - 1);
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
			auto mask = std::make_shared<awst::IntegerConstant>();
			mask->sourceLocation = m_loc;
			mask->wtype = awst::WType::biguintType();
			mask->value = maskVal.str();
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
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = targetType;
		cast->expr = std::move(result);
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
	auto toBytes = std::make_shared<awst::ReinterpretCast>();
	toBytes->sourceLocation = m_loc;
	toBytes->wtype = awst::WType::bytesType();
	toBytes->expr = std::move(_expr);

	auto result = leftPadToN(std::move(toBytes), _byteWidth);
	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (targetType != awst::WType::bytesType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = targetType;
		cast->expr = std::move(result);
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
	auto nConst = std::make_shared<awst::IntegerConstant>();
	nConst->sourceLocation = m_loc;
	nConst->wtype = awst::WType::uint64Type();
	nConst->value = std::to_string(_n);

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

	auto nConst2 = std::make_shared<awst::IntegerConstant>();
	nConst2->sourceLocation = m_loc;
	nConst2->wtype = awst::WType::uint64Type();
	nConst2->value = std::to_string(_n);

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = m_loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(lenExpr);
	offset->right = std::move(nConst2);
	offset->op = awst::UInt64BinaryOperator::Sub;

	auto nConst3 = std::make_shared<awst::IntegerConstant>();
	nConst3->sourceLocation = m_loc;
	nConst3->wtype = awst::WType::uint64Type();
	nConst3->value = std::to_string(_n);

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
	auto offsetConst = std::make_shared<awst::IntegerConstant>();
	offsetConst->sourceLocation = m_loc;
	offsetConst->wtype = awst::WType::uint64Type();
	offsetConst->value = std::to_string(8 - _n);

	auto widthConst = std::make_shared<awst::IntegerConstant>();
	widthConst->sourceLocation = m_loc;
	widthConst->wtype = awst::WType::uint64Type();
	widthConst->value = std::to_string(_n);

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
