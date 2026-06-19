/// @file TypeCoercion.cpp
/// Centralised type coercion / conversion utilities for AWST expressions.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <libsolutil/Numeric.h>

#include <libsolidity/ast/AST.h>

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
		auto idBytes = awst::makeAsUInt64(std::move(_expr), _loc);
		auto itob = awst::makeItob(std::move(idBytes), _loc);
		auto cat = awst::makeLeftPad(std::move(itob), 24, _loc);
		return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
	}

	// uint64 → biguint: itob then reinterpret as biguint
	if (_expr->wtype == awst::WType::uint64Type() && _targetType == awst::WType::biguintType())
	{
		auto itob = awst::makeItob(std::move(_expr), _loc);
		return awst::makeAsBiguint(std::move(itob), _loc);
	}

	// biguint → uint64: safely extract lower 64 bits
	// btoi only works on ≤8 bytes, but biguint from ABI-decoded uint256 is 32 bytes.
	// Approach: prepend 8 zero bytes, then extract last 8 bytes, then btoi.
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		// reinterpret biguint → bytes
		auto toBytes = awst::makeAsBytes(std::move(_expr), _loc);

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
			// biguint → bytes[N]: cast to bytes (strips leading zeros for
			// minimal encoding) then LEFT-pad to N bytes, preserving the
			// integer value's big-endian representation. Mirrors Solidity's
			// implicit hex-literal → bytesN conversion (e.g. passing
			// `0x000...ca35...` to a `bytes32` parameter). Without this,
			// biguint args flow through unchanged and downstream `concat`/
			// `extract` operations read the wrong byte width — the
			// minimal-encoding form (often <32 B). See
			// ecrecover/failing_ecrecover_invalid_input_proper.sol.
			if (_expr->wtype == awst::WType::biguintType())
			{
				auto toBytes = awst::makeAsBytes(std::move(_expr), _loc);
				auto padded = awst::makeLeftPadToN(std::move(toBytes), n, _loc);
				return awst::makeReinterpretCast(std::move(padded), _targetType, _loc);
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

	// `promoted` is referenced 3 times below (cond LHS, add LHS, conditional
	// else-branch). If it's a side-effecting expression — notably `this.h()`
	// in `return this.h();` from an int<N>-returning function whose body
	// mutates transient storage via this.g() — naïve AST duplication would
	// emit the callsub three times, running the side effects thrice. Bind
	// to a fresh temp variable via an AssignmentExpression so the value is
	// computed once and the three subsequent references read the temp.
	//
	// CommaExpression wraps the (assign-then-conditional) sequence so this
	// remains an Expression (signExtendToUint256's return contract).
	static int s_signExtTempId = 0;
	std::string tempName = "__signext_tmp_" + std::to_string(++s_signExtTempId);
	auto tempVar = awst::makeVarExpression(tempName, awst::WType::biguintType(), _loc);
	auto bind = awst::makeAssignmentExpression(
		tempVar, std::move(promoted), _loc, awst::WType::biguintType());

	// All subsequent uses reference the temp var (a fresh VarExpression each
	// time — puya treats local-var reads as cheap and never re-evaluates the
	// underlying side-effecting source).
	auto tempRead = [&]() {
		return awst::makeVarExpression(tempName, awst::WType::biguintType(), _loc);
	};

	// threshold = 2^(N-1)
	solidity::u256 threshold = solidity::u256(1) << (_bits - 1);
	// 2^256 as a string (u256 can't hold it, it overflows to 0)
	// offset = 2^256 - 2^N: compute using 512-bit int to avoid overflow
	boost::multiprecision::uint512_t pow256_wide(kPow2_256);
	boost::multiprecision::uint512_t offset_wide = pow256_wide - (boost::multiprecision::uint512_t(1) << _bits);
	std::string offsetStr = offset_wide.str();

	auto threshConst = awst::makeIntegerConstant(threshold.str(), _loc, awst::WType::biguintType());

	auto cond = awst::makeNumericCompare(tempRead(), awst::NumericComparison::Gte, threshConst, _loc);

	auto offsetConst = awst::makeIntegerConstant(offsetStr, _loc, awst::WType::biguintType());

	// `add` = masked + (2^256 - 2^N). This branch only runs when the masked
	// value is in [2^(N-1), 2^N - 1], so the sum lands in [2^256 - 2^(N-1),
	// 2^256 - 1] — always < 2^256. A `mod 2^256` here would be a guaranteed
	// no-op, so it's omitted.
	auto add = awst::makeBigUIntBinOp(tempRead(), awst::BigUIntBinaryOperator::Add, std::move(offsetConst), _loc);

	auto conditional = awst::makeConditional(
		std::move(cond), std::move(add), tempRead(), awst::WType::biguintType(), _loc);

	auto comma = awst::makeCommaExpression(awst::WType::biguintType(), _loc);
	comma->expressions.push_back(std::move(bind));
	comma->expressions.push_back(std::move(conditional));
	return comma;
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendToUint64(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	// Full-width or invalid: nothing to extend (int64 decode is already the
	// 8-byte two's-complement; >=64 has no high bits to fill).
	if (_bits == 0 || _bits >= 64)
		return _value;

	// Bind the (possibly side-effecting, e.g. box-backed) source to a temp so
	// the two reads below evaluate it once. MASK to the low `_bits` bits first
	// (`mod 2^bits`) so this is correct whether the source is the minimal sub-word
	// form OR already sign-extended to 64 bits (e.g. an ABI-decoded int8 param):
	// the add below assumes value ∈ [0, 2^bits), which only holds post-mask.
	static int s_signExt64TempId = 0;
	std::string tempName = "__signext64_tmp_" + std::to_string(++s_signExt64TempId);
	auto masked = awst::makeUInt64BinOp(
		std::move(_value), awst::UInt64BinaryOperator::Mod,
		awst::makeIntegerConstant(std::uint64_t(1) << _bits, _loc), _loc);
	auto bind = awst::makeAssignmentExpression(
		awst::makeVarExpression(tempName, awst::WType::uint64Type(), _loc),
		std::move(masked), _loc, awst::WType::uint64Type());
	auto tempRead = [&]() {
		return awst::makeVarExpression(tempName, awst::WType::uint64Type(), _loc);
	};

	// value ∈ [0, 2^bits-1]; reinterpret as signed: if the N-bit sign bit is
	// set, add (2^64 - 2^bits) to fill the high bits. value + (2^64 - 2^bits)
	// stays < 2^64 for every value in range, so the add never overflows even if
	// both conditional arms are evaluated.
	std::uint64_t threshold = std::uint64_t(1) << (_bits - 1);
	std::uint64_t offset = ~((std::uint64_t(1) << _bits) - 1); // 2^64 - 2^bits

	auto cond = awst::makeNumericCompare(
		tempRead(), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(threshold, _loc), _loc);
	auto extended = awst::makeUInt64BinOp(
		tempRead(), awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(offset, _loc), _loc);
	auto conditional = awst::makeConditional(
		std::move(cond), std::move(extended), tempRead(),
		awst::WType::uint64Type(), _loc);

	auto comma = awst::makeCommaExpression(awst::WType::uint64Type(), _loc);
	comma->expressions.push_back(std::move(bind));
	comma->expressions.push_back(std::move(conditional));
	return comma;
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendSignedElement(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _solElemType,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_solElemType))
		_solElemType = &udvt->underlyingType();
	// Only biguint-backed signed elements (64 < N < 256) need extension. int256
	// is already canonical two's complement; <=64-bit elements are uint64-backed
	// and carry their own sign handling (a 256-bit extension would mis-type them).
	if (auto const* intType = dynamic_cast<IntegerType const*>(_solElemType))
		if (intType->isSigned() && intType->numBits() > 64 && intType->numBits() < 256)
			return signExtendToUint256(std::move(_value), intType->numBits(), _loc);
	return _value;
}

std::shared_ptr<awst::Expression> TypeCoercion::checkedIndexToUint64(
	std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
	std::shared_ptr<awst::Expression> _idx,
	awst::SourceLocation const& _loc
)
{
	// Array index → uint64 with a bounds PRE-check: a wide (biguint) index >= 2^64 is always out of
	// bounds (no AVM array reaches 2^64 elements), so assert it fits in uint64 BEFORE truncating —
	// else `arr[2^128]` silently truncates the high bits and reads arr[low-64-bits] instead of
	// reverting (the downstream `index < length` check only sees the truncated value). Pins to a
	// temp so a side-effecting index (`a[--i]`) evaluates once.
	if (_idx && _idx->wtype == awst::WType::biguintType())
	{
		static int s_ckIdxCtr = 0;
		std::string nm = "__ckidx_" + std::to_string(s_ckIdxCtr++);
		_preStmts.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, awst::WType::biguintType(), _loc), std::move(_idx), _loc));
		auto fits = awst::makeNumericCompare(
			awst::makeVarExpression(nm, awst::WType::biguintType(), _loc),
			awst::NumericComparison::Lt,
			awst::makeIntegerConstant("18446744073709551616", _loc, awst::WType::biguintType()), _loc);
		_preStmts.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(fits), _loc, "array index out of bounds"), _loc));
		_idx = awst::makeVarExpression(nm, awst::WType::biguintType(), _loc);
	}
	return implicitNumericCast(std::move(_idx), awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendSignedWiden(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _srcSolType,
	solidity::frontend::Type const* _tgtSolType,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;
	// Widening a SIGNED intN to a wider SIGNED intM drops the sign in our value model: sub-word
	// ints are uint64-backed (so int8->int16 is a uint64->uint64 no-op) and the registry/cast
	// zero-extends. Re-extend from the SOURCE width. Covers both target tiers (≤64 uint64-backed,
	// >64 biguint-backed). No-op for unsigned, narrowing, same-width, or non-int operands.
	auto asInt = [](Type const* t) -> IntegerType const* {
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(t))
			t = &udvt->underlyingType();
		return dynamic_cast<IntegerType const*>(t);
	};
	auto const* srcInt = _srcSolType ? asInt(_srcSolType) : nullptr;
	auto const* tgtInt = _tgtSolType ? asInt(_tgtSolType) : nullptr;
	if (!_value || !srcInt || !tgtInt) return _value;
	if (!srcInt->isSigned() || !tgtInt->isSigned()) return _value;
	if (srcInt->numBits() >= tgtInt->numBits()) return _value;
	if (tgtInt->numBits() > 64 && _value->wtype == awst::WType::biguintType())
		return signExtendToUint256(std::move(_value), srcInt->numBits(), _loc);
	if (_value->wtype == awst::WType::uint64Type())
		return signExtendToUint64(std::move(_value), srcInt->numBits(), _loc);
	return _value;
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

	// A string longer than the target width can't be right-padded into bytes[N]
	// without dropping bytes. Solidity rejects such conversions up front, so this
	// is a defensive guard: fall through (nullptr) rather than silently truncate.
	if (static_cast<int>(sc->value.size()) > _n)
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

std::optional<std::string> TypeCoercion::intSelectorName(
	solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_type))
		_type = &udvt->underlyingType();
	if (auto const* intT = dynamic_cast<IntegerType const*>(_type))
	{
		unsigned bits = intT->numBits();
		// <=64-bit collapses to uint64; >64-bit keeps its exact width. Signedness
		// is always dropped (the callee names int128 as "uint128").
		return bits <= 64 ? std::string("uint64") : ("uint" + std::to_string(bits));
	}
	return std::nullopt;
}

std::optional<std::string> TypeCoercion::intSelectorReturnName(
	solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_type))
		_type = &udvt->underlyingType();
	if (auto const* intT = dynamic_cast<IntegerType const*>(_type))
	{
		// A SIGNED integer RETURN is encoded as the full 256-bit two's complement
		// (sign-extended), so the callee names it "uint256" regardless of width
		// (verified via TEAL: int8/int64/int128/int256 returns are all "uint256").
		// Unsigned returns use the same exact-width rule as params. The param vs
		// return asymmetry for signed ints is why this is separate from
		// intSelectorName.
		if (intT->isSigned())
			return std::string("uint256");
		unsigned bits = intT->numBits();
		return bits <= 64 ? std::string("uint64") : ("uint" + std::to_string(bits));
	}
	return std::nullopt;
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

		// For static arrays of DYNAMIC-content elements (e.g.
		// `uint[][2]` — fixed[2] of `uint[]`), `computeEncodedElementSize`
		// returns 0 because the element size isn't statically fixed. A
		// zero-byte default crashes the `static_array_replace_dynamic_element`
		// helper at the first push (it tries to read uint16 offset from
		// position 2 of an empty buffer). Build the proper ARC4 default:
		// N×2-byte offset header pointing at each inner element's default
		// encoding, followed by the inner defaults concatenated as the
		// tail.
		//
		// Gate on `arc4IsDynamic`, NOT on `encodedSize == 0` — the size
		// helper also returns 0 for some fixed-content cases that
		// `computeEncodedElementSize` doesn't enumerate (notably
		// `bool[N]` where the element is `arc4.bool`, which is
		// bit-packed and never appears in the switch). Those still want
		// the existing empty-bytes default; applying the dyn-encoding
		// shape would corrupt storage shape and break round-trip
		// (regresses `storage/delete_overlapping_transient_*_storage_array_delete_different_base_type`).
		if (arc4IsDynamic(_type))
		{
			if (auto enc = arc4DefaultEncoding(_type))
				return awst::makeBytesConstant(
					std::move(*enc), _loc, awst::BytesEncoding::Base16, _type);
		}

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


std::vector<uint8_t> TypeCoercion::intLiteralToBytesN(std::string const& _decimal, int _n)
{
	// Low-N-byte big-endian form of a non-negative integer literal. solc already
	// parsed it to a u256, so re-parse with boost::multiprecision instead of a
	// hand-rolled base-256 multiply (bytesN has N<=32, so it fits; also handles
	// 0x… literals). Bytes beyond N drop; missing high bytes stay 0.
	std::vector<uint8_t> out(_n > 0 ? static_cast<size_t>(_n) : 0, 0);
	if (_n <= 0 || _decimal.empty())
		return out;
	solidity::u256 v(_decimal);
	for (int i = 0; i < _n; ++i)
	{
		out[static_cast<size_t>(_n - 1 - i)] =
			static_cast<uint8_t>(static_cast<uint64_t>(v & 0xFFu));
		v >>= 8;
	}
	return out;
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
				// Decode to the SOURCE's native width (not hardcoded uint64): a >64-bit
				// source element (e.g. uint128 in `uint256[] = [2**100]`) would otherwise
				// be truncated to its low 64 bits. Then widen to the target's native width.
				auto const* srcNative = srcArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();
				auto const* widerNative = tgtArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();

				std::shared_ptr<awst::Expression> bodyBytes;
				for (auto const& elem : newArr->values)
				{
					auto decode = awst::makeARC4Decode(elem, srcNative, _loc);
					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != srcNative)
						nativeVal = implicitNumericCast(std::move(nativeVal), widerNative, _loc);

					auto encode = awst::makeARC4Encode(std::move(nativeVal), dynArr->elementType(), _loc);

					auto encBytes = awst::makeAsBytes(std::move(encode), _loc);

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
					auto elemBytes = awst::makeAsBytes(elem, _loc);
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

					auto srcBytes = awst::makeAsBytes(std::move(_expr), _loc);
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
				auto const* srcNative = srcArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();
				auto const* widerNative = tgtArc4->n() <= 64
					? awst::WType::uint64Type()
					: awst::WType::biguintType();

				auto widened = awst::makeNewArray(_targetType, _loc);
				for (auto const& elem : newArr->values)
				{
					// Decode to the SOURCE's native width (>64-bit source elements must
					// not be truncated to uint64), then widen to the target's width.
					auto decode = awst::makeARC4Decode(elem, srcNative, _loc);

					std::shared_ptr<awst::Expression> nativeVal = std::move(decode);
					if (widerNative != srcNative)
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
					auto elemBytes = awst::makeAsBytes(elem, _loc);
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
				return awst::makeBytesConstant(
					intLiteralToBytesN(intConst->value, N), _loc,
					awst::BytesEncoding::Base16, _targetType);
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
						auto srcBytes = awst::makeAsBytes(std::move(_expr), _loc);
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
		auto idBytes = awst::makeAsUInt64(std::move(_expr), _loc);
		auto itob = awst::makeItob(std::move(idBytes), _loc);
		auto cat = awst::makeLeftPad(std::move(itob), 24, _loc);
		return awst::makeReinterpretCast(std::move(cat), _targetType, _loc);
	}

	// account → application: extract last 8 bytes (app_id) via btoi
	// Only meaningful for addresses built from our convention (\x00*24 + app_id).
	if (_targetType == awst::WType::applicationType()
		&& _expr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeAsBytes(std::move(_expr), _loc);
		auto btoi = awst::makeWord32ToUInt64(std::move(toBytes), _loc);
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

} // namespace puyasol::builder
