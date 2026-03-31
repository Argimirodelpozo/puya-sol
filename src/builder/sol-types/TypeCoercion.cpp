/// @file TypeCoercion.cpp
/// Centralised type coercion / conversion utilities for AWST expressions.

#include "builder/sol-types/TypeCoercion.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <libsolutil/Numeric.h>

namespace puyasol::builder
{

// ── Numeric ──────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> TypeCoercion::implicitNumericCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	if (!_expr || !_targetType || _expr->wtype == _targetType)
		return _expr;

	// uint64 → biguint: itob then reinterpret as biguint
	if (_expr->wtype == awst::WType::uint64Type() && _targetType == awst::WType::biguintType())
	{
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_expr));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	}

	// biguint → uint64: safely extract lower 64 bits
	// btoi only works on ≤8 bytes, but biguint from ABI-decoded uint256 is 32 bytes.
	// Approach: prepend 8 zero bytes, then extract last 8 bytes, then btoi.
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		// reinterpret biguint → bytes
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(_expr);

		// bzero(8) — 8 zero bytes padding
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = _loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

		auto padding = std::make_shared<awst::IntrinsicCall>();
		padding->sourceLocation = _loc;
		padding->wtype = awst::WType::bytesType();
		padding->opCode = "bzero";
		padding->stackArgs.push_back(std::move(eight));

		// concat(padding, bytes) → padded
		auto padded = std::make_shared<awst::IntrinsicCall>();
		padded->sourceLocation = _loc;
		padded->wtype = awst::WType::bytesType();
		padded->opCode = "concat";
		padded->stackArgs.push_back(std::move(padding));
		padded->stackArgs.push_back(std::move(toBytes));

		// len(padded) → paddedLen
		auto paddedLen = std::make_shared<awst::IntrinsicCall>();
		paddedLen->sourceLocation = _loc;
		paddedLen->wtype = awst::WType::uint64Type();
		paddedLen->opCode = "len";
		paddedLen->stackArgs.push_back(padded);

		// paddedLen - 8 → offset
		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = _loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";

		auto offset = std::make_shared<awst::UInt64BinaryOperation>();
		offset->sourceLocation = _loc;
		offset->wtype = awst::WType::uint64Type();
		offset->left = std::move(paddedLen);
		offset->op = awst::UInt64BinaryOperator::Sub;
		offset->right = std::move(eight2);

		// extract3(padded, offset, 8) → last 8 bytes
		auto eight3 = std::make_shared<awst::IntegerConstant>();
		eight3->sourceLocation = _loc;
		eight3->wtype = awst::WType::uint64Type();
		eight3->value = "8";

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(std::move(padded));
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(std::move(eight3));

		// btoi(last8) → uint64
		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(extract));
		return btoi;
	}

	return _expr;
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendToUint256(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	// Promote to biguint if needed
	auto promoted = implicitNumericCast(
		std::move(_value), awst::WType::biguintType(), _loc);

	// Mask to N bits: value & (2^N - 1)
	solidity::u256 maskVal = (solidity::u256(1) << _bits) - 1;
	auto maskConst = std::make_shared<awst::IntegerConstant>();
	maskConst->sourceLocation = _loc;
	maskConst->wtype = awst::WType::biguintType();
	maskConst->value = maskVal.str();

	auto masked = std::make_shared<awst::BigUIntBinaryOperation>();
	masked->sourceLocation = _loc;
	masked->wtype = awst::WType::biguintType();
	masked->left = promoted;
	masked->op = awst::BigUIntBinaryOperator::BitAnd;
	masked->right = std::move(maskConst);
	promoted = masked;

	// threshold = 2^(N-1)
	solidity::u256 threshold = solidity::u256(1) << (_bits - 1);
	// 2^256 as a string (u256 can't hold it, it overflows to 0)
	static const std::string pow256Str = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
	// offset = 2^256 - 2^N: compute using 512-bit int to avoid overflow
	boost::multiprecision::uint512_t pow256_wide(pow256Str);
	boost::multiprecision::uint512_t offset_wide = pow256_wide - (boost::multiprecision::uint512_t(1) << _bits);
	std::string offsetStr = offset_wide.str();

	auto threshConst = std::make_shared<awst::IntegerConstant>();
	threshConst->sourceLocation = _loc;
	threshConst->wtype = awst::WType::biguintType();
	threshConst->value = threshold.str();

	auto cond = std::make_shared<awst::NumericComparisonExpression>();
	cond->sourceLocation = _loc;
	cond->wtype = awst::WType::boolType();
	cond->lhs = promoted;
	cond->op = awst::NumericComparison::Gte;
	cond->rhs = threshConst;

	auto offsetConst = std::make_shared<awst::IntegerConstant>();
	offsetConst->sourceLocation = _loc;
	offsetConst->wtype = awst::WType::biguintType();
	offsetConst->value = offsetStr;

	auto add = std::make_shared<awst::BigUIntBinaryOperation>();
	add->sourceLocation = _loc;
	add->wtype = awst::WType::biguintType();
	add->left = promoted;
	add->op = awst::BigUIntBinaryOperator::Add;
	add->right = std::move(offsetConst);

	// Mod 2^256 to keep within 32 bytes
	auto pow256Const = std::make_shared<awst::IntegerConstant>();
	pow256Const->sourceLocation = _loc;
	pow256Const->wtype = awst::WType::biguintType();
	pow256Const->value = pow256Str;

	auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
	mod->sourceLocation = _loc;
	mod->wtype = awst::WType::biguintType();
	mod->left = std::move(add);
	mod->op = awst::BigUIntBinaryOperator::Mod;
	mod->right = std::move(pow256Const);

	auto ternary = std::make_shared<awst::ConditionalExpression>();
	ternary->sourceLocation = _loc;
	ternary->wtype = awst::WType::biguintType();
	ternary->condition = std::move(cond);
	ternary->trueExpr = std::move(mod);
	ternary->falseExpr = promoted;

	return ternary;
}

// ── Bytes ────────────────────────────────────────────────────────

std::shared_ptr<awst::BytesConstant> TypeCoercion::stringToBytesN(
	awst::Expression const* _src,
	awst::WType const* _targetType,
	int _n,
	awst::SourceLocation const& _loc
)
{
	auto const* sc = dynamic_cast<awst::StringConstant const*>(_src);
	if (!sc || _n <= 0)
		return nullptr;

	auto padded = std::make_shared<awst::BytesConstant>();
	padded->sourceLocation = _loc;
	padded->wtype = _targetType;
	padded->encoding = awst::BytesEncoding::Base16;
	padded->value.assign(sc->value.begin(), sc->value.end());
	padded->value.resize(_n, 0); // right-pad with zeroes
	return padded;
}

std::shared_ptr<awst::ReinterpretCast> TypeCoercion::reinterpretCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = _targetType;
	cast->expr = std::move(_expr);
	return cast;
}

std::shared_ptr<awst::Expression> TypeCoercion::stringToBytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	auto const* sc = dynamic_cast<awst::StringConstant const*>(_expr.get());
	if (!sc)
		return _expr;

	auto bc = std::make_shared<awst::BytesConstant>();
	bc->sourceLocation = _loc;
	bc->wtype = awst::WType::bytesType();
	bc->encoding = awst::BytesEncoding::Base16;
	bc->value.assign(sc->value.begin(), sc->value.end());
	return bc;
}

// ── ARC4 / ABI ───────────────────────────────────────────────────

std::string TypeCoercion::wtypeToABIName(awst::WType const* _type)
{
	if (_type == awst::WType::arc4BoolType())
		return "bool";

	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	{
		auto const* uintN = static_cast<awst::ARC4UIntN const*>(_type);
		return "uint" + std::to_string(uintN->n());
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* sa = static_cast<awst::ARC4StaticArray const*>(_type);
		return wtypeToABIName(sa->elementType()) + "[" + std::to_string(sa->arraySize()) + "]";
	}
	case awst::WTypeKind::ARC4DynamicArray:
	{
		auto const* da = static_cast<awst::ARC4DynamicArray const*>(_type);
		if (!da->arc4Alias().empty())
			return da->arc4Alias();
		return wtypeToABIName(da->elementType()) + "[]";
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = static_cast<awst::ARC4Struct const*>(_type);
		std::string result = "(";
		bool first = true;
		for (auto const& [name, fieldType]: st->fields())
		{
			if (!first) result += ",";
			result += wtypeToABIName(fieldType);
			first = false;
		}
		result += ")";
		return result;
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tp = static_cast<awst::ARC4Tuple const*>(_type);
		std::string result = "(";
		bool first = true;
		for (auto const* elemType: tp->types())
		{
			if (!first) result += ",";
			result += wtypeToABIName(elemType);
			first = false;
		}
		result += ")";
		return result;
	}
	default:
		return _type->name();
	}
}

// ── Defaults ─────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> TypeCoercion::makeDefaultValue(
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	if (!_type)
	{
		auto val = std::make_shared<awst::BytesConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::bytesType();
		val->encoding = awst::BytesEncoding::Base16;
		val->value = {};
		return val;
	}

	// Bool → BoolConstant
	if (_type == awst::WType::boolType())
	{
		auto val = std::make_shared<awst::BoolConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::boolType();
		val->value = false;
		return val;
	}

	// Integer types → IntegerConstant
	if (_type == awst::WType::uint64Type())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::uint64Type();
		val->value = "0";
		return val;
	}
	if (_type == awst::WType::biguintType())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::biguintType();
		val->value = "0";
		return val;
	}
	if (_type->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = _type;
		val->value = "0";
		return val;
	}

	// Tuple → TupleExpression with component defaults (recursive)
	if (_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(_type);
		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = _type;
		for (auto const* componentType: tupleType->types())
			tuple->items.push_back(makeDefaultValue(componentType, _loc));
		return tuple;
	}

	// ARC4Struct → NewStruct with field defaults (recursive)
	if (_type->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		auto expr = std::make_shared<awst::NewStruct>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		for (auto const& [name, fieldType]: structType->fields())
			expr->values[name] = makeDefaultValue(fieldType, _loc);
		return expr;
	}

	// ReferenceArray → NewArray with default elements
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(_type);
		auto arr = std::make_shared<awst::NewArray>();
		arr->sourceLocation = _loc;
		arr->wtype = _type;
		if (refArr->arraySize().has_value())
		{
			for (int i = 0; i < refArr->arraySize().value(); ++i)
				arr->values.push_back(makeDefaultValue(refArr->elementType(), _loc));
		}
		return arr;
	}

	// ARC4StaticArray → BytesConstant of correct encoded size (zero-filled)
	if (_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		int encodedSize = computeEncodedElementSize(_type);
		auto val = std::make_shared<awst::BytesConstant>();
		val->sourceLocation = _loc;
		val->wtype = _type;
		val->encoding = awst::BytesEncoding::Base16;
		if (encodedSize > 0)
			val->value = std::vector<uint8_t>(static_cast<size_t>(encodedSize), 0);
		else
			val->value = {};
		return val;
	}

	// ARC4DynamicArray → empty with 2-byte length header (0x0000)
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray)
	{
		auto val = std::make_shared<awst::BytesConstant>();
		val->sourceLocation = _loc;
		val->wtype = _type;
		val->encoding = awst::BytesEncoding::Base16;
		val->value = {0x00, 0x00};
		return val;
	}

	// Everything else (bytes, string, account, ARC4 types, etc.)
	auto val = std::make_shared<awst::BytesConstant>();
	val->sourceLocation = _loc;
	val->wtype = _type;
	val->encoding = awst::BytesEncoding::Base16;

	if (_type == awst::WType::accountType())
		val->value = std::vector<uint8_t>(32, 0);
	else if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
	{
		if (bytesType->length().has_value())
			val->value = std::vector<uint8_t>(static_cast<size_t>(bytesType->length().value()), 0);
		else
			val->value = {};
	}
	else
		val->value = {};

	return val;
}

int TypeCoercion::computeEncodedElementSize(awst::WType const* _type)
{
	if (!_type)
		return 0;

	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
		return static_cast<awst::ARC4UIntN const*>(_type)->n() / 8;
	case awst::WTypeKind::ARC4UFixedNxM:
		return static_cast<awst::ARC4UFixedNxM const*>(_type)->n() / 8;
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		int total = 0;
		for (auto const& [name, fieldType]: structType->fields())
		{
			int fieldSize = computeEncodedElementSize(fieldType);
			if (fieldSize == 0)
				return 0;
			total += fieldSize;
		}
		return total;
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		int elemSize = computeEncodedElementSize(arr->elementType());
		if (elemSize == 0)
			return 0;
		return arr->arraySize() * elemSize;
	}
	case awst::WTypeKind::ReferenceArray:
	{
		auto const* arr = static_cast<awst::ReferenceArray const*>(_type);
		if (!arr->arraySize())
			return 0;
		int elemSize = computeEncodedElementSize(arr->elementType());
		if (elemSize == 0)
			return 0;
		return *arr->arraySize() * elemSize;
	}
	case awst::WTypeKind::ARC4DynamicArray:
		return 0;
	case awst::WTypeKind::Bytes:
	{
		auto const* bytesType = static_cast<awst::BytesWType const*>(_type);
		if (bytesType->length())
			return *bytesType->length();
		return 0;
	}
	case awst::WTypeKind::Basic:
	{
		if (_type == awst::WType::biguintType())
			return 32;
		if (_type == awst::WType::uint64Type())
			return 8;
		if (_type == awst::WType::boolType())
			return 8;
		if (_type == awst::WType::accountType())
			return 32;
		return 0;
	}
	default:
		return 0;
	}
}

std::shared_ptr<awst::Expression> TypeCoercion::coerceForAssignment(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (!_expr || !_targetType || _expr->wtype == _targetType)
		return _expr;

	// Numeric cast (uint64 ↔ biguint)
	_expr = implicitNumericCast(std::move(_expr), _targetType, _loc);
	if (_expr->wtype == _targetType)
		return _expr;

	// IntegerConstant → BytesConstant(bytes[N])
	if (_targetType->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_targetType);
		if (bytesType && bytesType->length().has_value())
		{
			int N = static_cast<int>(*bytesType->length());

			// IntegerConstant → bytes[N]
			if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
			{
				// Parse the decimal string to big-endian bytes
				std::vector<unsigned char> bytes(N, 0);
				std::string numStr = intConst->value;
				std::vector<unsigned char> bignum;
				for (char c : numStr)
				{
					int digit = c - '0';
					int carry = digit;
					for (auto& b : bignum)
					{
						int v = b * 10 + carry;
						b = static_cast<unsigned char>(v & 0xFF);
						carry = v >> 8;
					}
					while (carry > 0)
					{
						bignum.push_back(static_cast<unsigned char>(carry & 0xFF));
						carry >>= 8;
					}
				}
				// bignum is little-endian; copy to big-endian bytes
				for (size_t i = 0; i < bignum.size() && i < bytes.size(); ++i)
					bytes[bytes.size() - 1 - i] = bignum[i];

				auto bc = std::make_shared<awst::BytesConstant>();
				bc->sourceLocation = _loc;
				bc->wtype = _targetType;
				bc->encoding = awst::BytesEncoding::Base16;
				bc->value = std::move(bytes);
				return bc;
			}

			// String → bytes[N] (right-padded)
			if (auto padded = stringToBytesN(_expr.get(), _targetType, N, _loc))
				return padded;
		}

		// String/bytes-compatible → bytes via ReinterpretCast
		if (_expr->wtype == awst::WType::stringType()
			|| _expr->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = _targetType;
			cast->expr = std::move(_expr);
			return cast;
		}
	}

	// Account ↔ bytes[32]
	if (_targetType == awst::WType::accountType()
		&& (_expr->wtype->kind() == awst::WTypeKind::Bytes
			|| _expr->wtype == awst::WType::bytesType()))
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = _targetType;
		cast->expr = std::move(_expr);
		return cast;
	}
	if (_expr->wtype == awst::WType::accountType()
		&& _targetType->kind() == awst::WTypeKind::Bytes)
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = _targetType;
		cast->expr = std::move(_expr);
		return cast;
	}

	// uint64 → bool (0/non-0)
	if (_targetType == awst::WType::boolType()
		&& _expr->wtype == awst::WType::uint64Type())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(_expr);
		cmp->op = awst::NumericComparison::Ne;
		cmp->rhs = std::move(zero);
		return cmp;
	}

	return _expr;
}

} // namespace puyasol::builder
