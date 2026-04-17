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

		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
		return cast;
	}

	// biguint → uint64: safely extract lower 64 bits
	// btoi only works on ≤8 bytes, but biguint from ABI-decoded uint256 is 32 bytes.
	// Approach: prepend 8 zero bytes, then extract last 8 bytes, then btoi.
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		// reinterpret biguint → bytes
		auto toBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

		// bzero(8) — 8 zero bytes padding
		auto eight = awst::makeIntegerConstant("8", _loc);

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
		auto eight2 = awst::makeIntegerConstant("8", _loc);

		auto offset = awst::makeUInt64BinOp(std::move(paddedLen), awst::UInt64BinaryOperator::Sub, std::move(eight2), _loc);

		// extract3(padded, offset, 8) → last 8 bytes
		auto eight3 = awst::makeIntegerConstant("8", _loc);

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

	// String / bytes constant → fixed-size bytes[N]: right-pad to N bytes.
	if (auto const* fbType = dynamic_cast<awst::BytesWType const*>(_targetType))
	{
		if (fbType->length().has_value() && *fbType->length() > 0)
		{
			int n = static_cast<int>(*fbType->length());
			if (auto padded = stringToBytesN(_expr.get(), _targetType, n, _loc))
				return padded;
			if (auto const* bc = dynamic_cast<awst::BytesConstant const*>(_expr.get()))
			{
				if (static_cast<int>(bc->value.size()) <= n)
				{
					auto val = bc->value;
					val.resize(static_cast<size_t>(n), 0);
					return awst::makeBytesConstant(
						std::move(val), _loc, awst::BytesEncoding::Base16, _targetType);
				}
			}
		}
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

	// For 256-bit signed types, the value is already in two's complement form
	// (from our signed arithmetic wrapping). No sign-extension needed.
	if (_bits == 256)
		return promoted;

	// Mask to N bits: value & (2^N - 1). Skip for 256-bit (already full width).
	if (_bits < 256)
	{
		solidity::u256 maskVal = (solidity::u256(1) << _bits) - 1;
		auto maskConst = awst::makeIntegerConstant(maskVal.str(), _loc, awst::WType::biguintType());

		auto masked = std::make_shared<awst::BigUIntBinaryOperation>();
		masked->sourceLocation = _loc;
		masked->wtype = awst::WType::biguintType();
		masked->left = promoted;
		masked->op = awst::BigUIntBinaryOperator::BitAnd;
		masked->right = std::move(maskConst);
		promoted = masked;
	}

	// threshold = 2^(N-1)
	solidity::u256 threshold = solidity::u256(1) << (_bits - 1);
	// 2^256 as a string (u256 can't hold it, it overflows to 0)
	// offset = 2^256 - 2^N: compute using 512-bit int to avoid overflow
	boost::multiprecision::uint512_t pow256_wide(kPow2_256);
	boost::multiprecision::uint512_t offset_wide = pow256_wide - (boost::multiprecision::uint512_t(1) << _bits);
	std::string offsetStr = offset_wide.str();

	auto threshConst = awst::makeIntegerConstant(threshold.str(), _loc, awst::WType::biguintType());

	auto cond = awst::makeNumericCompare(promoted, awst::NumericComparison::Gte, threshConst, _loc);

	auto offsetConst = awst::makeIntegerConstant(offsetStr, _loc, awst::WType::biguintType());

	auto add = std::make_shared<awst::BigUIntBinaryOperation>();
	add->sourceLocation = _loc;
	add->wtype = awst::WType::biguintType();
	add->left = promoted;
	add->op = awst::BigUIntBinaryOperator::Add;
	add->right = std::move(offsetConst);

	// Mod 2^256 to keep within 32 bytes
	auto pow256Const = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

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

	std::vector<uint8_t> val(sc->value.begin(), sc->value.end());
	val.resize(_n, 0); // right-pad with zeroes
	return awst::makeBytesConstant(
		std::move(val), _loc, awst::BytesEncoding::Base16, _targetType);
}

std::shared_ptr<awst::ReinterpretCast> TypeCoercion::reinterpretCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	auto cast = awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
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

	return awst::makeBytesConstant(
		std::vector<uint8_t>(sc->value.begin(), sc->value.end()), _loc);
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
		return awst::makeBytesConstant({}, _loc);

	// Bool → BoolConstant
	if (_type == awst::WType::boolType())
	{
		return awst::makeBoolConstant(false, _loc);
	}

	// Integer types → IntegerConstant
	if (_type == awst::WType::uint64Type())
	{
		auto val = awst::makeIntegerConstant("0", _loc);
		return val;
	}
	if (_type == awst::WType::biguintType())
	{
		auto val = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
		return val;
	}
	if (_type->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto const* arc4UInt = static_cast<awst::ARC4UIntN const*>(_type);
		// ARC4 zero: N/8 zero bytes as BytesConstant with ARC4UIntN type
		int numBytes = arc4UInt->n() / 8;
		return awst::makeBytesConstant(
			std::vector<uint8_t>(numBytes, 0), _loc, awst::BytesEncoding::Base16, _type);
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
			for (int64_t i = 0; i < refArr->arraySize().value(); ++i)
				arr->values.push_back(makeDefaultValue(refArr->elementType(), _loc));
		}
		return arr;
	}

	// ARC4StaticArray → BytesConstant of correct encoded size (zero-filled).
	// Puya's pushbytes has a ~4KB cap; for anything bigger, emit bzero(N) so
	// the zero region is allocated at runtime instead of baked into the bytecode.
	if (_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		int encodedSize = computeEncodedElementSize(_type);
		if (encodedSize > kLargeBytesRuntimeThreshold)
			return makeZeroBytesRuntime(encodedSize, _type, _loc);

		std::vector<uint8_t> val;
		if (encodedSize > 0)
			val.resize(static_cast<size_t>(encodedSize), 0);
		return awst::makeBytesConstant(
			std::move(val), _loc, awst::BytesEncoding::Base16, _type);
	}

	// ARC4DynamicArray → empty with 2-byte length header (0x0000)
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray)
		return awst::makeBytesConstant(
			{0x00, 0x00}, _loc, awst::BytesEncoding::Base16, _type);

	// Everything else (bytes, string, account, ARC4 types, etc.)
	std::vector<uint8_t> val;
	if (_type == awst::WType::accountType())
		val.assign(32, 0);
	else if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
	{
		if (bytesType->length().has_value())
		{
			int n = static_cast<int>(*bytesType->length());
			if (n > kLargeBytesRuntimeThreshold)
				return makeZeroBytesRuntime(n, _type, _loc);
			val.assign(static_cast<size_t>(n), 0);
		}
	}
	return awst::makeBytesConstant(
		std::move(val), _loc, awst::BytesEncoding::Base16, _type);
}

std::shared_ptr<awst::Expression> TypeCoercion::makeZeroBytesRuntime(
	int _n,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	auto size = awst::makeIntegerConstant(std::to_string(_n), _loc);

	auto bzero = std::make_shared<awst::IntrinsicCall>();
	bzero->sourceLocation = _loc;
	bzero->wtype = awst::WType::bytesType();
	bzero->opCode = "bzero";
	bzero->stackArgs.push_back(std::move(size));

	if (_targetType == awst::WType::bytesType())
		return bzero;

	auto cast = awst::makeReinterpretCast(std::move(bzero), _targetType, _loc);
	return cast;
}

std::shared_ptr<awst::Expression> TypeCoercion::prependArc4LengthHeader(
	std::shared_ptr<awst::Expression> _expr,
	int64_t /*_length*/,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	// Delegate to puya's ConvertArray lowering — it already knows how to add
	// the uint16 length header when going from ARC4StaticArray to
	// ARC4DynamicArray (and how to strip it in the reverse direction),
	// so we don't need to synthesise concat+reinterpret by hand.
	auto convert = std::make_shared<awst::ConvertArray>();
	convert->sourceLocation = _loc;
	convert->wtype = _targetType;
	convert->expr = std::move(_expr);
	return convert;
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

	// ARC4StaticArray<T, N> → ARC4DynamicArray<T>: prepend 2-byte length header.
	// Solidity allows implicit static→dynamic array conversions on assignment
	// (e.g. `uint8[] storage x = new uint8[5]`). Puya's ARC4 pipeline keeps
	// these as distinct types, so we materialise the conversion via a
	// ConvertArray node — puya lowers it to the right header+body layout.
	if (auto const* dynArr = dynamic_cast<awst::ARC4DynamicArray const*>(_targetType))
	{
		if (auto const* statArr = dynamic_cast<awst::ARC4StaticArray const*>(_expr->wtype))
		{
			// Element types aren't interned between TypeMapper calls, so we
			// compare structurally on the element name rather than pointer.
			if (statArr->elementType() && dynArr->elementType()
				&& statArr->elementType()->name() == dynArr->elementType()->name())
				return prependArc4LengthHeader(std::move(_expr), statArr->arraySize(), _targetType, _loc);

			// Narrower inline array literal (e.g. `[7,8,9]` typed uint8[3])
			// assigned into a wider-element dynamic target (e.g. `uint256[]`).
			// Widen each element via decode+encode, concat the widened bytes,
			// prepend the uint16 length header, reinterpret as the dynamic
			// target. NewArray-only to avoid re-evaluating impure sources.
			auto const* srcArc4 = dynamic_cast<awst::ARC4UIntN const*>(statArr->elementType());
			auto const* tgtArc4 = dynamic_cast<awst::ARC4UIntN const*>(dynArr->elementType());
			auto* newArr = dynamic_cast<awst::NewArray*>(_expr.get());
			if (newArr && srcArc4 && tgtArc4
				&& srcArc4->n() < tgtArc4->n()
				&& srcArc4->arc4Alias().empty() && tgtArc4->arc4Alias().empty())
			{
				auto const* widerNative = tgtArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();

				std::shared_ptr<awst::Expression> bodyBytes;
				for (auto const& elem : newArr->values)
				{
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = _loc;
					decode->wtype = awst::WType::uint64Type();
					decode->value = elem;
					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != awst::WType::uint64Type())
						nativeVal = implicitNumericCast(std::move(nativeVal), widerNative, _loc);

					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = _loc;
					encode->wtype = dynArr->elementType();
					encode->value = std::move(nativeVal);

					auto encBytes = awst::makeReinterpretCast(std::move(encode), awst::WType::bytesType(), _loc);

					if (!bodyBytes)
						bodyBytes = std::move(encBytes);
					else
					{
						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = _loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(bodyBytes));
						cat->stackArgs.push_back(std::move(encBytes));
						bodyBytes = std::move(cat);
					}
				}

				int N = static_cast<int>(statArr->arraySize());
				auto header = awst::makeBytesConstant(
					{static_cast<uint8_t>((N >> 8) & 0xFF),
					 static_cast<uint8_t>(N & 0xFF)},
					_loc);

				auto withHeader = std::make_shared<awst::IntrinsicCall>();
				withHeader->sourceLocation = _loc;
				withHeader->wtype = awst::WType::bytesType();
				withHeader->opCode = "concat";
				withHeader->stackArgs.push_back(std::move(header));
				withHeader->stackArgs.push_back(std::move(bodyBytes));

				auto cast = awst::makeReinterpretCast(std::move(withHeader), _targetType, _loc);
				return cast;
			}
		}
	}

	// ARC4StaticArray<T, M> → ARC4StaticArray<T, N> with M < N: Solidity
	// allows assigning a smaller fixed-size array into a larger one,
	// zero-filling the trailing slots. Puya's encoder rejects the
	// length-mismatched encoding outright, so we synthesise the wider
	// encoded value as `concat(src_bytes, bzero(diff))` and reinterpret
	// to the wider ARC4StaticArray type.
	if (auto const* targetStat = dynamic_cast<awst::ARC4StaticArray const*>(_targetType))
	{
		if (auto const* srcStat = dynamic_cast<awst::ARC4StaticArray const*>(_expr->wtype))
		{
			if (srcStat->elementType() && targetStat->elementType()
				&& srcStat->elementType()->name() == targetStat->elementType()->name()
				&& srcStat->arraySize() < targetStat->arraySize())
			{
				int elemSize = computeEncodedElementSize(srcStat->elementType());
				if (elemSize > 0)
				{
					int64_t diffElems = targetStat->arraySize() - srcStat->arraySize();
					int64_t diffBytes = diffElems * elemSize;

					auto srcBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

					auto padSize = awst::makeIntegerConstant(std::to_string(diffBytes), _loc);
					auto pad = std::make_shared<awst::IntrinsicCall>();
					pad->sourceLocation = _loc;
					pad->wtype = awst::WType::bytesType();
					pad->opCode = "bzero";
					pad->stackArgs.push_back(std::move(padSize));

					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = _loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(srcBytes));
					cat->stackArgs.push_back(std::move(pad));

					auto cast = awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
					return cast;
				}
			}

			// Same-length inline array literal with narrower element type —
			// Solidity infers the common-type of `[1,2,3,4]` as uint8[4],
			// but the target (e.g. `uint256[4] storage`) is wider. Only the
			// NewArray literal case is handled here to avoid re-evaluating
			// impure source expressions; other sources fall through to the
			// encoder which will fail with a clear type-mismatch error.
			auto const* srcArc4 = dynamic_cast<awst::ARC4UIntN const*>(srcStat->elementType());
			auto const* tgtArc4 = dynamic_cast<awst::ARC4UIntN const*>(targetStat->elementType());
			auto* newArr = dynamic_cast<awst::NewArray*>(_expr.get());
			if (newArr && srcArc4 && tgtArc4
				&& srcStat->arraySize() == targetStat->arraySize()
				&& srcArc4->n() < tgtArc4->n()
				&& srcArc4->arc4Alias().empty() && tgtArc4->arc4Alias().empty())
			{
				auto const* widerNative = tgtArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();

				auto widened = std::make_shared<awst::NewArray>();
				widened->sourceLocation = _loc;
				widened->wtype = _targetType;
				for (auto const& elem : newArr->values)
				{
					// Decode narrow ARC4 → uint64 (narrow types always ≤ 64 bits here)
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = _loc;
					decode->wtype = awst::WType::uint64Type();
					decode->value = elem;

					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != awst::WType::uint64Type())
						nativeVal = implicitNumericCast(std::move(nativeVal), widerNative, _loc);

					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = _loc;
					encode->wtype = targetStat->elementType();
					encode->value = std::move(nativeVal);
					widened->values.push_back(std::move(encode));
				}
				return widened;
			}
		}
	}

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

				return awst::makeBytesConstant(
					std::move(bytes), _loc, awst::BytesEncoding::Base16, _targetType);
			}

			// String → bytes[N] (right-padded)
			if (auto padded = stringToBytesN(_expr.get(), _targetType, N, _loc))
				return padded;
		}

		// String/bytes-compatible → bytes via ReinterpretCast.
		// For fixed-size bytes[N] targets coming from a narrower fixed
		// bytes[M] (M < N), Solidity right-pads the source with zeros to
		// produce N bytes. A bare ReinterpretCast leaves the source's M
		// bytes labelled as bytes[N], which decodes to the wrong width
		// at the call boundary; build the padded value explicitly.
		if (_expr->wtype == awst::WType::stringType()
			|| _expr->wtype->kind() == awst::WTypeKind::Bytes)
		{
			if (auto const* tw = dynamic_cast<awst::BytesWType const*>(_targetType))
			{
				if (tw->length().has_value())
				{
					int targetWidth = static_cast<int>(*tw->length());
					int sourceWidth = 0;
					if (auto const* sw = dynamic_cast<awst::BytesWType const*>(_expr->wtype))
						if (sw->length().has_value())
							sourceWidth = static_cast<int>(*sw->length());
					if (sourceWidth > 0 && sourceWidth < targetWidth)
					{
						auto srcBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

						int padBytes = targetWidth - sourceWidth;
						auto padSize = awst::makeIntegerConstant(std::to_string(padBytes), _loc);
						auto pad = std::make_shared<awst::IntrinsicCall>();
						pad->sourceLocation = _loc;
						pad->wtype = awst::WType::bytesType();
						pad->opCode = "bzero";
						pad->stackArgs.push_back(std::move(padSize));

						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = _loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(srcBytes));
						cat->stackArgs.push_back(std::move(pad));

						auto cast = awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
						return cast;
					}
				}
			}

			auto cast = awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
			return cast;
		}
	}

	// Account ↔ bytes[32]
	if (_targetType == awst::WType::accountType()
		&& (_expr->wtype->kind() == awst::WTypeKind::Bytes
			|| _expr->wtype == awst::WType::bytesType()))
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
		return cast;
	}
	if (_expr->wtype == awst::WType::accountType()
		&& _targetType->kind() == awst::WTypeKind::Bytes)
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
		return cast;
	}

	// application → account: encode the app id into a fake address of
	// the form (24 zero bytes ++ itob(app_id)). This round-trips
	// losslessly through the inverse `extract 24 8; btoi` we use in the
	// account→application path below, so `A a = new A(); a.f();` keeps
	// the original app id rather than the SHA512_256 on-chain address
	// (which is opaque and can't be recovered).
	if (_targetType == awst::WType::accountType()
		&& _expr->wtype == awst::WType::applicationType())
	{
		auto idBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::uint64Type(), _loc);

		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(idBytes));

		auto padSize = awst::makeIntegerConstant("24", _loc);
		auto pad = std::make_shared<awst::IntrinsicCall>();
		pad->sourceLocation = _loc;
		pad->wtype = awst::WType::bytesType();
		pad->opCode = "bzero";
		pad->stackArgs.push_back(std::move(padSize));

		auto cat = std::make_shared<awst::IntrinsicCall>();
		cat->sourceLocation = _loc;
		cat->wtype = awst::WType::bytesType();
		cat->opCode = "concat";
		cat->stackArgs.push_back(std::move(pad));
		cat->stackArgs.push_back(std::move(itob));

		auto accountCast = awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
		return accountCast;
	}

	// account → application: extract last 8 bytes (app_id) via btoi
	// Only meaningful for addresses built from our convention (\x00*24 + app_id).
	if (_targetType == awst::WType::applicationType()
		&& _expr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract";
		extract->immediates = {24, 8};
		extract->stackArgs.push_back(std::move(toBytes));

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(extract));

		auto appIdCast = awst::makeReinterpretCast(std::move(btoi), _targetType, _loc);
		return appIdCast;
	}

	// uint64 → bool (0/non-0)
	if (_targetType == awst::WType::boolType()
		&& _expr->wtype == awst::WType::uint64Type())
	{
		auto zero = awst::makeIntegerConstant("0", _loc);
		auto cmp = awst::makeNumericCompare(std::move(_expr), awst::NumericComparison::Ne, std::move(zero), _loc);
		return cmp;
	}

	return _expr;
}

} // namespace puyasol::builder
