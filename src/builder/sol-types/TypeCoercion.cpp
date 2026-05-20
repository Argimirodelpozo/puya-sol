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

	// application → account: encode the app id into a fake address of the
	// form `bzero(24) ++ itob(app_id)`. Solidity contract types (e.g. `A`)
	// type-map to `account` (Solidity treats contract values as addresses),
	// but `new A()` produces an `application` (uint64 app_id). When a
	// function declared `returns (A)` returns a `new A()` expression — or
	// any other application/account site mixing — this implicit cast
	// closes the gap. Round-trips losslessly with the inverse account →
	// application path in coerceForAssignment.
	if (_targetType == awst::WType::accountType()
		&& _expr->wtype == awst::WType::applicationType())
	{
		auto idBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::uint64Type(), _loc);
		auto itob = awst::makeItob(std::move(idBytes), _loc);
		auto cat = awst::makeLeftPad(std::move(itob), 24, _loc);
		return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
	}

	// uint64 → biguint: itob then reinterpret as biguint
	if (_expr->wtype == awst::WType::uint64Type() && _targetType == awst::WType::biguintType())
	{
		auto itob = awst::makeItob(std::move(_expr), _loc);
		return awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
	}

	// biguint → uint64: safely extract lower 64 bits
	// btoi only works on ≤8 bytes, but biguint from ABI-decoded uint256 is 32 bytes.
	// Approach: prepend 8 zero bytes, then extract last 8 bytes, then btoi.
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		// reinterpret biguint → bytes
		auto toBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

		// concat(bzero(8), bytes) → padded; then extract3 last 8 → btoi.
		auto padded = awst::makeLeftPad(std::move(toBytes), 8, _loc);
		auto extract = awst::makeExtractLastN(std::move(padded), 8, _loc);
		return awst::makeBtoi(std::move(extract), _loc);
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

		auto masked = awst::makeBigUIntBinOp(promoted, awst::BigUIntBinaryOperator::BitAnd, std::move(maskConst), _loc);
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

	auto add = awst::makeBigUIntBinOp(promoted, awst::BigUIntBinaryOperator::Add, std::move(offsetConst), _loc);

	// Mod 2^256 to keep within 32 bytes
	auto pow256Const = makePow256(_loc);

	auto mod = awst::makeBigUIntBinOp(std::move(add), awst::BigUIntBinaryOperator::Mod, std::move(pow256Const), _loc);

	return awst::makeConditional(
		std::move(cond), std::move(mod), promoted, awst::WType::biguintType(), _loc);
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
		if (!sa->arc4Alias().empty())
			return sa->arc4Alias();
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
		return awst::makeFalse(_loc);
	}

	// arc4.bool → 1-byte BytesConstant 0x00. (Without this, arc4Bool falls
	// through to the bytes-fallback branch and returns *empty* bytes — which
	// is the wrong wire encoding for an arc4 bool and trips downstream
	// getbit/length checks. The bool-array and bool-struct-field workarounds
	// cover their own paths, but a direct arc4Bool local var would surface
	// this if hit.)
	if (_type == awst::WType::arc4BoolType())
	{
		return awst::makeBytesConstant(
			std::vector<uint8_t>{0}, _loc, awst::BytesEncoding::Base16, _type);
	}

	// Integer types → IntegerConstant
	if (_type == awst::WType::uint64Type())
	{
		auto val = awst::makeZero(_loc);
		return val;
	}
	if (_type == awst::WType::biguintType())
	{
		auto val = awst::makeBiguintConstant("0", _loc);
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
		auto tuple = awst::makeTupleExpression(_type, _loc);
		for (auto const* componentType: tupleType->types())
			tuple->items.push_back(makeDefaultValue(componentType, _loc));
		return tuple;
	}

	// ARC4Struct → BytesConstant of zeros at the encoded width (preferred)
	// or NewStruct with field defaults (fallback for dynamic structs).
	//
	// Why prefer the BytesConstant: puya's NewStruct encoder has a bug
	// when one or more fields are arc4.bool. The encoder packs consecutive
	// bools into bits via setbit on a running bytes buffer, but starts
	// from `bytec_1 // 0x` (empty bytes) instead of bzero(1). The first
	// getbit/setbit then errors with "index beyond byteslice", which makes
	// every default-struct read on a mapping that contains bool fields
	// (Hub.SpokeData, Hub.Asset, etc.) panic at runtime. By emitting the
	// zero-filled bytes literal at the correct encoded size we skip puya's
	// encoder entirely for the all-zero case — what comes out is what puya
	// *should* have produced for an all-zero default. See puyabug.md §2.
	if (_type->kind() == awst::WTypeKind::ARC4Struct)
	{
		int encodedSize = computeEncodedElementSize(_type);
		if (encodedSize > 0)
		{
			if (encodedSize > kLargeBytesRuntimeThreshold)
				return makeZeroBytesRuntime(encodedSize, _type, _loc);
			return awst::makeBytesConstant(
				std::vector<uint8_t>(static_cast<size_t>(encodedSize), 0),
				_loc, awst::BytesEncoding::Base16, _type);
		}

		// Dynamic-size struct (a field has variable encoding). Use the
		// `arc4DefaultEncoding` helper which builds the correct head+tail
		// byte layout including dynamic-field offsets — avoids puya's
		// buggy NewStruct encoder path (which mispacks bools onto an empty
		// bytes buffer instead of bzero(1)) and produces the right struct
		// size (head + sum of dynamic-field empty tails) for cases like
		// `struct { uint a; uint8 b; mapping(K=>V) c; bool d; }` where the
		// mapping is bytes-typed at the AWST level.
		if (auto def = arc4DefaultEncoding(_type))
			return awst::makeBytesConstant(
				std::move(*def), _loc, awst::BytesEncoding::Base16, _type);

		// Fallback (some field's default isn't statically computable):
		// NewStruct + recursive defaults. May still hit the puya
		// bool-packing bug if any field is arc4.bool — not reached today
		// by AAVE V4 / the bundled tests.
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		auto expr = awst::makeNewStruct(_type, _loc);
		for (auto const& [name, fieldType]: structType->fields())
			expr->values[name] = makeDefaultValue(fieldType, _loc);
		return expr;
	}

	// ReferenceArray → NewArray with default elements
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(_type);
		auto arr = awst::makeNewArray(_type, _loc);
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
	auto bzero = awst::makeBzero(_n, _loc);
	if (_targetType == awst::WType::bytesType())
		return bzero;
	return awst::makeReinterpretCast(std::move(bzero), _targetType, _loc);
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
	return awst::makeConvertArray(std::move(_expr), _targetType, _loc);
}

bool TypeCoercion::arc4IsDynamic(awst::WType const* _type)
{
	if (!_type)
		return false;
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4DynamicArray:
		return true;
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		return arc4IsDynamic(arr->elementType());
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = static_cast<awst::ARC4Struct const*>(_type);
		for (auto const& [name, ft]: st->fields())
			if (arc4IsDynamic(ft))
				return true;
		return false;
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tu = static_cast<awst::ARC4Tuple const*>(_type);
		for (auto const* ft: tu->types())
			if (arc4IsDynamic(ft))
				return true;
		return false;
	}
	case awst::WTypeKind::Bytes:
	{
		auto const* bw = static_cast<awst::BytesWType const*>(_type);
		return !bw->length().has_value();
	}
	default:
		return false;
	}
}

std::optional<std::vector<uint8_t>> TypeCoercion::arc4DefaultEncoding(
	awst::WType const* _type
)
{
	if (!_type)
		return std::nullopt;
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	{
		auto const* u = static_cast<awst::ARC4UIntN const*>(_type);
		return std::vector<uint8_t>(static_cast<size_t>(u->n() / 8), 0);
	}
	case awst::WTypeKind::ARC4UFixedNxM:
	{
		auto const* u = static_cast<awst::ARC4UFixedNxM const*>(_type);
		return std::vector<uint8_t>(static_cast<size_t>(u->n() / 8), 0);
	}
	case awst::WTypeKind::ARC4DynamicArray:
		return std::vector<uint8_t>{0, 0};
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		auto const* elemT = arr->elementType();
		auto N = static_cast<int64_t>(arr->arraySize());
		if (N < 0)
			return std::nullopt;
		auto elemDefault = arc4DefaultEncoding(elemT);
		if (!elemDefault)
			return std::nullopt;

		std::vector<uint8_t> result;
		if (arc4IsDynamic(elemT))
		{
			int64_t headSize = N * 2;
			int64_t tailSize = static_cast<int64_t>(elemDefault->size());
			result.reserve(static_cast<size_t>(headSize + N * tailSize));
			for (int64_t i = 0; i < N; ++i)
			{
				int64_t off = headSize + i * tailSize;
				result.push_back(static_cast<uint8_t>((off >> 8) & 0xFF));
				result.push_back(static_cast<uint8_t>(off & 0xFF));
			}
			for (int64_t i = 0; i < N; ++i)
				result.insert(result.end(), elemDefault->begin(), elemDefault->end());
		}
		else
		{
			result.reserve(static_cast<size_t>(N * static_cast<int64_t>(elemDefault->size())));
			for (int64_t i = 0; i < N; ++i)
				result.insert(result.end(), elemDefault->begin(), elemDefault->end());
		}
		return result;
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = static_cast<awst::ARC4Struct const*>(_type);
		// Pre-compute each field's default and total head size so dynamic
		// field offsets can be embedded.
		struct FieldEnc { bool dynamic; std::vector<uint8_t> bytes; };
		std::vector<FieldEnc> encs;
		encs.reserve(st->fields().size());
		int64_t headSize = 0;
		for (auto const& [name, ft]: st->fields())
		{
			auto fd = arc4DefaultEncoding(ft);
			if (!fd)
				return std::nullopt;
			bool dyn = arc4IsDynamic(ft);
			headSize += dyn ? 2 : static_cast<int64_t>(fd->size());
			encs.push_back({dyn, std::move(*fd)});
		}

		std::vector<uint8_t> head;
		std::vector<uint8_t> tail;
		head.reserve(static_cast<size_t>(headSize));
		int64_t tailOff = headSize;
		for (auto const& fe: encs)
		{
			if (fe.dynamic)
			{
				head.push_back(static_cast<uint8_t>((tailOff >> 8) & 0xFF));
				head.push_back(static_cast<uint8_t>(tailOff & 0xFF));
				tail.insert(tail.end(), fe.bytes.begin(), fe.bytes.end());
				tailOff += static_cast<int64_t>(fe.bytes.size());
			}
			else
			{
				head.insert(head.end(), fe.bytes.begin(), fe.bytes.end());
			}
		}
		head.insert(head.end(), tail.begin(), tail.end());
		return head;
	}
	case awst::WTypeKind::Bytes:
	{
		auto const* bw = static_cast<awst::BytesWType const*>(_type);
		if (bw->length().has_value())
			return std::vector<uint8_t>(static_cast<size_t>(*bw->length()), 0);
		return std::vector<uint8_t>{0, 0};
	}
	case awst::WTypeKind::Basic:
	{
		if (_type == awst::WType::biguintType())
			return std::vector<uint8_t>(32, 0);
		if (_type == awst::WType::uint64Type())
			return std::vector<uint8_t>(8, 0);
		if (_type == awst::WType::boolType())
			return std::vector<uint8_t>{0};
		if (_type == awst::WType::arc4BoolType())
			return std::vector<uint8_t>{0};
		if (_type == awst::WType::accountType())
			return std::vector<uint8_t>(32, 0);
		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
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
		// ARC4 packs consecutive `arc4.bool` fields into shared bytes
		// (8 bools per byte). All other fields are byte-aligned. Walk
		// the field list, accumulating bool runs and flushing them as
		// `ceil(run_length / 8)` bytes whenever a non-bool field
		// interrupts the run (or at the end). If any non-bool field
		// is dynamic (size 0), the whole struct is dynamic.
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		int total = 0;
		int boolRun = 0;
		auto flushBoolRun = [&]() {
			if (boolRun > 0)
			{
				total += (boolRun + 7) / 8;
				boolRun = 0;
			}
		};
		for (auto const& [name, fieldType]: structType->fields())
		{
			if (fieldType == awst::WType::arc4BoolType())
			{
				boolRun++;
				continue;
			}
			flushBoolRun();
			int fieldSize = computeEncodedElementSize(fieldType);
			if (fieldSize == 0)
				return 0;
			total += fieldSize;
		}
		flushBoolRun();
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
					auto decode = awst::makeARC4Decode(elem, awst::WType::uint64Type(), _loc);
					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != awst::WType::uint64Type())
						nativeVal = implicitNumericCast(std::move(nativeVal), widerNative, _loc);

					auto encode = awst::makeARC4Encode(std::move(nativeVal), dynArr->elementType(), _loc);

					auto encBytes = awst::makeReinterpretCast(std::move(encode), awst::WType::bytesType(), _loc);

					if (!bodyBytes)
						bodyBytes = std::move(encBytes);
					else
						bodyBytes = awst::makeConcat(std::move(bodyBytes), std::move(encBytes), _loc);
				}

				int N = static_cast<int>(statArr->arraySize());
				auto header = awst::makeBytesConstant(
					{static_cast<uint8_t>((N >> 8) & 0xFF),
					 static_cast<uint8_t>(N & 0xFF)},
					_loc);

				auto withHeader = awst::makeConcat(std::move(header), std::move(bodyBytes), _loc);
				return awst::makeReinterpretCast(std::move(withHeader), _targetType, _loc);
			}

			// Signed variant: `int8[K]` literal → `int16[]` (any signed
			// widening). Sign-extend at byte level (prepend 0xFF/0x00 pad
			// based on bit-7 of each element's first byte), concat into
			// body bytes, prepend uint16 length header.
			bool const srcSigned =
				srcArc4 && srcArc4->arc4Alias().size() >= 3
				&& srcArc4->arc4Alias().substr(0, 3) == "int";
			bool const tgtSigned =
				tgtArc4 && tgtArc4->arc4Alias().size() >= 3
				&& tgtArc4->arc4Alias().substr(0, 3) == "int";
			if (newArr && srcArc4 && tgtArc4
				&& srcArc4->n() < tgtArc4->n()
				&& srcArc4->n() % 8 == 0 && tgtArc4->n() % 8 == 0
				&& srcSigned && tgtSigned)
			{
				int const padBytes = (tgtArc4->n() - srcArc4->n()) / 8;
				std::shared_ptr<awst::Expression> bodyBytes;
				for (auto const& elem : newArr->values)
				{
					auto elemBytes = awst::makeReinterpretCast(
						elem, awst::WType::bytesType(), _loc);
					auto signByte = awst::makeExtract3(
						elemBytes,
						awst::makeIntegerConstant(0, _loc),
						awst::makeIntegerConstant(1, _loc), _loc);
					auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
					auto isNeg = awst::makeNumericCompare(
						std::move(signByteVal),
						awst::NumericComparison::Gte,
						awst::makeIntegerConstant(128, _loc), _loc);
					std::vector<uint8_t> ffPad(padBytes, 0xFFu);
					std::vector<uint8_t> zeroPad(padBytes, 0x00u);
					auto prepend = awst::makeConditional(
						std::move(isNeg),
						awst::makeBytesConstant(std::move(ffPad), _loc),
						awst::makeBytesConstant(std::move(zeroPad), _loc),
						awst::WType::bytesType(), _loc);
					auto widenedBytes = awst::makeConcat(
						std::move(prepend), std::move(elemBytes), _loc);
					if (!bodyBytes) bodyBytes = std::move(widenedBytes);
					else bodyBytes = awst::makeConcat(
						std::move(bodyBytes), std::move(widenedBytes), _loc);
				}

				int N = static_cast<int>(statArr->arraySize());
				auto header = awst::makeBytesConstant(
					{static_cast<uint8_t>((N >> 8) & 0xFF),
					 static_cast<uint8_t>(N & 0xFF)},
					_loc);
				auto withHeader = awst::makeConcat(
					std::move(header), std::move(bodyBytes), _loc);
				return awst::makeReinterpretCast(std::move(withHeader), _targetType, _loc);
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
					auto cat = awst::makeRightPad(std::move(srcBytes), diffBytes, _loc);
					return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
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

				auto widened = awst::makeNewArray(_targetType, _loc);
				for (auto const& elem : newArr->values)
				{
					// Decode narrow ARC4 → uint64 (narrow types always ≤ 64 bits here)
					auto decode = awst::makeARC4Decode(elem, awst::WType::uint64Type(), _loc);

					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != awst::WType::uint64Type())
						nativeVal = implicitNumericCast(std::move(nativeVal), widerNative, _loc);

					auto encode = awst::makeARC4Encode(std::move(nativeVal), targetStat->elementType(), _loc);
					widened->values.push_back(std::move(encode));
				}
				return widened;
			}

			// Signed version: `intM[K]` literal → `intN[K]` (M < N).
			// puya's ARC4 encoder rejects uint64 > 2^N-1 for arc4.uintN
			// targets, so we can't widen via uint64 sign-OR'ing. Instead,
			// build the wider element bytes directly per slot: for each
			// element, prepend (N-M)/8 sign bytes (0xFF or 0x00) to the
			// source byte slice. Final concat → reinterpret as target.
			bool const srcSigned =
				srcArc4 && srcArc4->arc4Alias().size() >= 3
				&& srcArc4->arc4Alias().substr(0, 3) == "int";
			bool const tgtSigned =
				tgtArc4 && tgtArc4->arc4Alias().size() >= 3
				&& tgtArc4->arc4Alias().substr(0, 3) == "int";
			if (newArr && srcArc4 && tgtArc4
				&& srcStat->arraySize() == targetStat->arraySize()
				&& srcArc4->n() < tgtArc4->n()
				&& srcArc4->n() % 8 == 0 && tgtArc4->n() % 8 == 0
				&& srcSigned && tgtSigned)
			{
				int const padBytes = (tgtArc4->n() - srcArc4->n()) / 8;
				std::shared_ptr<awst::Expression> result;
				for (auto const& elem : newArr->values)
				{
					// elem is an arc4.intM literal (BytesConstant of srcBytes).
					auto elemBytes = awst::makeReinterpretCast(
						elem, awst::WType::bytesType(), _loc);
					// Sign-extend per element by inspecting bit 7 of byte 0.
					auto signByte = awst::makeExtract3(
						elemBytes,
						awst::makeIntegerConstant(0, _loc),
						awst::makeIntegerConstant(1, _loc), _loc);
					auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
					auto isNeg = awst::makeNumericCompare(
						std::move(signByteVal),
						awst::NumericComparison::Gte,
						awst::makeIntegerConstant(128, _loc), _loc);
					std::vector<uint8_t> ffPad(padBytes, 0xFFu);
					std::vector<uint8_t> zeroPad(padBytes, 0x00u);
					auto prepend = awst::makeConditional(
						std::move(isNeg),
						awst::makeBytesConstant(std::move(ffPad), _loc),
						awst::makeBytesConstant(std::move(zeroPad), _loc),
						awst::WType::bytesType(), _loc);
					auto widenedBytes = awst::makeConcat(
						std::move(prepend), std::move(elemBytes), _loc);
					if (!result) result = std::move(widenedBytes);
					else result = awst::makeConcat(std::move(result), std::move(widenedBytes), _loc);
				}
				return awst::makeReinterpretCast(std::move(result), _targetType, _loc);
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
						auto cat = awst::makeRightPad(std::move(srcBytes), padBytes, _loc);
						return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
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
		auto itob = awst::makeItob(std::move(idBytes), _loc);
		auto cat = awst::makeLeftPad(std::move(itob), 24, _loc);
		return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
	}

	// account → application: extract last 8 bytes (app_id) via btoi
	// Only meaningful for addresses built from our convention (\x00*24 + app_id).
	if (_targetType == awst::WType::applicationType()
		&& _expr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
		auto extract = awst::makeExtract(std::move(toBytes), 24, 8, _loc);
		auto btoi = awst::makeBtoi(std::move(extract), _loc);
		return awst::makeReinterpretCast(std::move(btoi), _targetType, _loc);
	}

	// uint64 → bool (0/non-0)
	if (_targetType == awst::WType::boolType()
		&& _expr->wtype == awst::WType::uint64Type())
	{
		auto zero = awst::makeZero(_loc);
		auto cmp = awst::makeNumericCompare(std::move(_expr), awst::NumericComparison::Ne, std::move(zero), _loc);
		return cmp;
	}

	return _expr;
}

std::shared_ptr<awst::Expression> TypeCoercion::tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (_value->wtype != awst::WType::uint64Type()) return nullptr;
	auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(_targetType);
	if (!arc4) return nullptr;
	int const bits = arc4->n();
	if (bits >= 64 || bits % 8 != 0) return nullptr;
	int const nBytes = bits / 8;
	auto bytes = awst::makeItob(std::move(_value), _loc);
	auto low = awst::makeExtract3(std::move(bytes),
		awst::makeIntegerConstant(8 - nBytes, _loc),
		awst::makeIntegerConstant(nBytes, _loc), _loc);
	return awst::makeReinterpretCast(std::move(low), _targetType, _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::tryWidenArc4StaticArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	awst::SourceLocation const& _loc)
{
	auto const* srcArr = dynamic_cast<awst::ARC4StaticArray const*>(_sourceType);
	auto const* tgtArr = dynamic_cast<awst::ARC4StaticArray const*>(_targetType);
	if (!srcArr || !tgtArr) return nullptr;
	if (srcArr->arraySize() != tgtArr->arraySize()) return nullptr;
	auto const* srcInt = dynamic_cast<awst::ARC4UIntN const*>(srcArr->elementType());
	auto const* tgtInt = dynamic_cast<awst::ARC4UIntN const*>(tgtArr->elementType());
	if (!srcInt || !tgtInt) return nullptr;
	int const srcBits = srcInt->n();
	int const tgtBits = tgtInt->n();
	if (srcBits >= tgtBits || srcBits % 8 != 0 || tgtBits % 8 != 0) return nullptr;
	int const srcBytes = srcBits / 8;
	int const tgtBytes = tgtBits / 8;
	int const padBytes = tgtBytes - srcBytes;
	int const count = static_cast<int>(srcArr->arraySize());

	// Sign-ness: "intN" alias → signed; "uintN" alias → unsigned.
	bool const isSigned =
		srcInt->arc4Alias().size() >= 3 && srcInt->arc4Alias().substr(0, 3) == "int";

	std::shared_ptr<awst::Expression> result;
	for (int i = 0; i < count; ++i)
	{
		// Per-element source bytes: extract3(src, i*srcBytes, srcBytes).
		auto srcByte = awst::makeExtract3(
			_mkSourceBytes(),
			awst::makeIntegerConstant(i * srcBytes, _loc),
			awst::makeIntegerConstant(srcBytes, _loc),
			_loc);

		std::shared_ptr<awst::Expression> widened;
		if (isSigned)
		{
			// Sign-extend: prepend 0xFF*padBytes if the high byte's bit 7 is
			// set, else 0x00*padBytes. We look at the first byte of the slice
			// (always the high byte in big-endian ARC4 encoding).
			auto signByte = awst::makeExtract3(
				srcByte,
				awst::makeIntegerConstant(0, _loc),
				awst::makeIntegerConstant(1, _loc),
				_loc);
			auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
			auto isNeg = awst::makeNumericCompare(
				std::move(signByteVal),
				awst::NumericComparison::Gte,
				awst::makeIntegerConstant(128, _loc),
				_loc);
			std::vector<uint8_t> ffPad(padBytes, 0xFFu);
			std::vector<uint8_t> zeroPad(padBytes, 0x00u);
			auto prepend = awst::makeConditional(
				std::move(isNeg),
				awst::makeBytesConstant(std::move(ffPad), _loc),
				awst::makeBytesConstant(std::move(zeroPad), _loc),
				awst::WType::bytesType(),
				_loc);
			widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
		}
		else
		{
			// Unsigned: prepend zeros.
			auto prepend = awst::makeBzero(padBytes, _loc);
			widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
		}

		if (!result) result = std::move(widened);
		else result = awst::makeConcat(std::move(result), std::move(widened), _loc);
	}

	return awst::makeReinterpretCast(std::move(result), _targetType, _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::tryWidenArc4DynamicArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	std::function<void(std::shared_ptr<awst::Statement>)> _emit,
	awst::SourceLocation const& _loc)
{
	auto const* srcArr = dynamic_cast<awst::ARC4DynamicArray const*>(_sourceType);
	auto const* tgtArr = dynamic_cast<awst::ARC4DynamicArray const*>(_targetType);
	if (!srcArr || !tgtArr) return nullptr;
	auto const* srcInt = dynamic_cast<awst::ARC4UIntN const*>(srcArr->elementType());
	auto const* tgtInt = dynamic_cast<awst::ARC4UIntN const*>(tgtArr->elementType());
	if (!srcInt || !tgtInt) return nullptr;
	int const srcBits = srcInt->n();
	int const tgtBits = tgtInt->n();
	if (srcBits >= tgtBits || srcBits % 8 != 0 || tgtBits % 8 != 0) return nullptr;
	int const srcBytes = srcBits / 8;
	int const padBytes = (tgtBits - srcBits) / 8;

	bool const isSigned =
		srcInt->arc4Alias().size() >= 3 && srcInt->arc4Alias().substr(0, 3) == "int";

	static int s_dwCounter = 0;
	int const n = s_dwCounter++;
	auto u64 = awst::WType::uint64Type();
	auto bytesT = awst::WType::bytesType();
	std::string const lenN = "__dwiden_len_" + std::to_string(n);
	std::string const idxN = "__dwiden_i_" + std::to_string(n);
	std::string const resN = "__dwiden_res_" + std::to_string(n);

	// __dwiden_len = extract_uint16(srcBytes, 0)
	auto lenExtract = awst::makeExtractUInt16(
		_mkSourceBytes(), awst::makeIntegerConstant(0, _loc), _loc, u64);
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(lenN, u64, _loc), std::move(lenExtract), _loc));

	// __dwiden_i = 0
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::makeIntegerConstant(0, _loc), _loc));

	// __dwiden_res = extract3(itob(len), 6, 2)  // 2-byte length prefix
	auto itobLen = awst::makeItob(
		awst::makeVarExpression(lenN, u64, _loc), _loc);
	auto lenPrefix = awst::makeExtract3(std::move(itobLen),
		awst::makeIntegerConstant(6, _loc),
		awst::makeIntegerConstant(2, _loc), _loc);
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(resN, bytesT, _loc), std::move(lenPrefix), _loc));

	// Loop body
	auto body = awst::makeBlock(_loc);

	// offset = 2 + __dwiden_i * srcBytes
	auto idxRead = awst::makeVarExpression(idxN, u64, _loc);
	auto offMul = awst::makeUInt64BinOp(std::move(idxRead),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(srcBytes, _loc), _loc);
	auto offsetExpr = awst::makeUInt64BinOp(
		awst::makeIntegerConstant(2, _loc),
		awst::UInt64BinaryOperator::Add,
		std::move(offMul), _loc);

	// srcByte = extract3(srcBytes, offset, srcBytes)
	auto srcByte = awst::makeExtract3(_mkSourceBytes(),
		std::move(offsetExpr),
		awst::makeIntegerConstant(srcBytes, _loc), _loc);

	std::shared_ptr<awst::Expression> widened;
	if (isSigned)
	{
		auto signByte = awst::makeExtract3(srcByte,
			awst::makeIntegerConstant(0, _loc),
			awst::makeIntegerConstant(1, _loc), _loc);
		auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
		auto isNeg = awst::makeNumericCompare(
			std::move(signByteVal),
			awst::NumericComparison::Gte,
			awst::makeIntegerConstant(128, _loc), _loc);
		std::vector<uint8_t> ffPad(padBytes, 0xFFu);
		std::vector<uint8_t> zeroPad(padBytes, 0x00u);
		auto prepend = awst::makeConditional(
			std::move(isNeg),
			awst::makeBytesConstant(std::move(ffPad), _loc),
			awst::makeBytesConstant(std::move(zeroPad), _loc),
			bytesT, _loc);
		widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
	}
	else
	{
		widened = awst::makeConcat(
			awst::makeBzero(padBytes, _loc),
			std::move(srcByte), _loc);
	}

	// __dwiden_res = concat(__dwiden_res, widened)
	auto resAppend = awst::makeConcat(
		awst::makeVarExpression(resN, bytesT, _loc),
		std::move(widened), _loc);
	body->body.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(resN, bytesT, _loc),
		std::move(resAppend), _loc));

	// __dwiden_i = __dwiden_i + 1
	auto idxInc = awst::makeUInt64BinOp(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(1, _loc), _loc);
	body->body.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(idxN, u64, _loc),
		std::move(idxInc), _loc));

	// while __dwiden_i < __dwiden_len { body }
	auto loopCond = awst::makeNumericCompare(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::NumericComparison::Lt,
		awst::makeVarExpression(lenN, u64, _loc), _loc);
	_emit(awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	return awst::makeReinterpretCast(
		awst::makeVarExpression(resN, bytesT, _loc),
		_targetType, _loc);
}

} // namespace puyasol::builder
