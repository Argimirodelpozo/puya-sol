/// @file TypeCoercion.cpp
/// Centralised type coercion / conversion utilities for AWST expressions.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/itxn/ApplicationTarget.h"
#include "awst/TupleValue.h"
#include "Logger.h"
#include "awst/NameGen.h"
#include "builder/sol-ast/StorageRefPointer.h"

#include <libsolidity/ast/TypeProvider.h>
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/TypeMapper.h"

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
	if (!_expr || !_targetType
		|| awst::structurallyEquivalent(_expr->wtype, _targetType))
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

	// biguint/uint64 → account: a bare address literal (`0x9BA1…`, 40 hex digits)
	// type-maps to biguint, but the assignment/param target is `account`; nothing
	// coerced it, so puya rejected the store ("assignment target type differs").
	// Right-align the integer into a 32-byte address (12 zero bytes ++ 20-byte
	// value), mirroring the explicit `address(uint)` cast. Ubiquitous in real
	// contracts (hardcoded router/multisig/fee/dead addresses).
	if (_targetType == awst::WType::accountType()
		&& (_expr->wtype == awst::WType::biguintType()
			|| _expr->wtype == awst::WType::uint64Type()))
	{
		std::shared_ptr<awst::Expression> asBytes;
		if (_expr->wtype == awst::WType::uint64Type())
			asBytes = awst::makeItob(std::move(_expr), _loc);
		else
			asBytes = awst::makeAsBytes(std::move(_expr), _loc);
		auto padded = awst::makeLeftPad(std::move(asBytes), 32, _loc);   // prepend 32 zero bytes
		auto last32 = awst::makeExtractLastN(std::move(padded), 32, _loc);
		return awst::makeReinterpretCast(std::move(last32), awst::WType::accountType(), _loc);
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
		return awst::makeBiguintToUInt64(std::move(_expr), _loc);

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
			// uint64 → bytes[N]: itob (8-byte big-endian) then LEFT-pad to N,
			// same shape as the biguint case above. A small integer/hex literal
			// like `records[0x0]` (key type bytes32) is an IntegerConstant of
			// wtype uint64; without this it flowed through unchanged and the
			// key-bytes step (makeKeyBytes fallback) reinterpret-cast a scalar
			// uint64 to bytes — an invalid cast puya rejects ("unsupported type
			// cast from uint64 to bytes"). itob+leftPad yields the same 32-byte
			// value as `bytes32(0)` / a `bytes32` key param, so keys byte-match.
			if (_expr->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeItob(std::move(_expr), _loc);
				auto padded = awst::makeLeftPadToN(std::move(itob), n, _loc);
				return awst::makeReinterpretCast(std::move(padded), _targetType, _loc);
			}
		}
	}

	return _expr;
}

std::shared_ptr<awst::Expression> TypeCoercion::encodeReturnElement(
	std::shared_ptr<awst::Expression> _value,
	ReturnWireElem const& _plan,
	awst::SourceLocation const& _loc,
	bool _asmWrap,
	bool _wire
)
{
	if (!_value)
		return _value;
	if (_plan.masked)
	{
		// Unsigned sub-word: mask the native uint64 to the declared width (Pass 5),
		// then at the wire boundary encode it as arc4.uint<bits>.
		uint64_t mask = (uint64_t(1) << _plan.bits) - 1;
		_value = awst::makeUInt64BinOp(std::move(_value), awst::UInt64BinaryOperator::BitAnd,
			awst::makeIntegerConstant(mask, _loc), _loc);
		if (!_wire || !_plan.encoded || !_plan.wireType
			|| _plan.wireType == _plan.nativeType)
			return _value;
		return awst::makeARC4Encode(std::move(_value), _plan.wireType, _loc);
	}
	if (!_plan.encoded)
		return _value;
	if (_plan.isSigned)
	{
		// Signed: sign-extend the (uint64-held) value to 256-bit two's complement,
		// then ARC4-encode to arc4.uint256 at the wire boundary.
		_value = signExtendToUint256(std::move(_value), _plan.bits, _loc);
		if (!_wire)
			return _value;
		return awst::makeARC4Encode(std::move(_value), _plan.wireType, _loc);
	}
	if (!_wire)
	{
		// Assembly is unchecked. Normalize a wide unsigned result before it is
		// threaded through modifiers; the eventual wire step then only encodes.
		if (_asmWrap && _plan.encoded
			&& _value->wtype == awst::WType::biguintType())
		{
			boost::multiprecision::cpp_int mod = 1;
			mod <<= _plan.bits;
			_value = awst::makeBigUIntBinOp(
				std::move(_value), awst::BigUIntBinaryOperator::Mod,
				awst::makeIntegerConstant(
					mod.str(), _loc, awst::WType::biguintType()), _loc);
		}
		if (_plan.nativeType && awst::isNumericWType(_plan.nativeType)
			&& awst::isNumericWType(_value->wtype)
			&& _value->wtype != _plan.nativeType)
			return implicitNumericCast(
				std::move(_value), _plan.nativeType, _loc);
		return _value;
	}
	// Sub-word unsigned already masked by the native pass (dispatch plan clears
	// `masked`): only the arc4.uint<bits> encode remains.
	if (_value->wtype == awst::WType::uint64Type() && _plan.wireType
		&& _plan.wireType != _plan.nativeType
		&& _plan.wireType->kind() == awst::WTypeKind::ARC4UIntN)
		return awst::makeARC4Encode(std::move(_value), _plan.wireType, _loc);
	// Unsigned biguint: ARC4-encode to arc4.uintN, guarded on biguint like Pass 2
	// (the expectedType coercion at the return site makes it biguint in practice).
	if (_value->wtype == awst::WType::biguintType())
	{
		if (_asmWrap)
		{
			// Asm bodies are UNCHECKED (Yul wraps mod 2^256); AVM biguint does not.
			// Wrap `value % 2^bits` before encoding so overflow matches EVM (Pass 2/3
			// encodeRet for asm functions).
			boost::multiprecision::cpp_int mod = 1;
			mod <<= _plan.bits;
			_value = awst::makeBigUIntBinOp(std::move(_value), awst::BigUIntBinaryOperator::Mod,
				awst::makeIntegerConstant(mod.str(), _loc, awst::WType::biguintType()), _loc);
		}
		return awst::makeARC4Encode(std::move(_value), _plan.wireType, _loc);
	}
	// Dynamic array (ReferenceArray) → its ARC4 array type (Pass 1).
	if (_plan.nativeType
		&& _plan.nativeType->kind() == awst::WTypeKind::ReferenceArray)
		return awst::makeARC4Encode(std::move(_value), _plan.wireType, _loc);
	return _value;
}

std::shared_ptr<awst::Expression> TypeCoercion::encodeReturnValue(
	TypeMapper& _typeMapper,
	std::shared_ptr<awst::Expression> _value,
	std::vector<ReturnWireElem> const& _plan,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _prepend,
	bool _asmWrap,
	bool _wire
)
{
	if (_plan.empty() || !_value)
		return _value;
	if (_plan.size() == 1)
		return encodeReturnElement(
			std::move(_value), _plan[0], _loc, _asmWrap, _wire);

	bool anyWork = false;
	for (auto const& p: _plan)
		if ((_wire && (p.encoded || p.masked))
			|| (!_wire && (p.isSigned || p.masked
				|| (_asmWrap && p.encoded))))
		{
			anyWork = true;
			break;
		}
	if (!anyWork)
		return _value;

	std::vector<awst::WType const*> wireTypes;
	for (auto const& p: _plan)
		wireTypes.push_back(_wire ? p.wireType : p.nativeType);
	auto items = awst::tupleItems(std::move(_value), _loc, &_prepend);
	assert(items.size() == _plan.size());
	auto tuple = awst::makeTupleExpression(
		_typeMapper.createType<awst::WTuple>(std::move(wireTypes)), _loc);
	for (size_t i = 0; i < items.size(); ++i)
		tuple->items.push_back(encodeReturnElement(
			std::move(items[i]), _plan[i], _loc, _asmWrap, _wire));
	return tuple;
}

std::shared_ptr<awst::Expression> TypeCoercion::calldataPointerValueRead(
	std::string const& _name,
	awst::SourceLocation const& _loc
)
{
	auto off = implicitNumericCast(
		awst::makeVarExpression("__cd_off_" + _name, awst::WType::biguintType(), _loc),
		awst::WType::uint64Type(), _loc);
	auto len = implicitNumericCast(
		awst::makeVarExpression("__cd_len_" + _name, awst::WType::biguintType(), _loc),
		awst::WType::uint64Type(), _loc);
	return awst::makeExtract3(
		awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), _loc),
		std::move(off), std::move(len), _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendToUint256(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	_value = implicitNumericCast(std::move(_value), awst::WType::biguintType(), _loc);
	if (_bits == 256) return _value;
	assert(_bits > 0 && _bits < 256);
	auto masked = awst::makeEvalOnce(awst::makeBigUIntBinOp(std::move(_value),
		awst::BigUIntBinaryOperator::BitAnd,
		awst::makeBiguintConstant(((solidity::u256(1) << _bits) - 1).str(), _loc), _loc), _loc);
	// Masking makes the conditional addition provably < 2^256.
	boost::multiprecision::uint512_t offset =
		boost::multiprecision::uint512_t(kPow2_256) - (boost::multiprecision::uint512_t(1) << _bits);
	return awst::makeConditional(isNegativeSigned(masked, _bits, _loc),
		awst::makeBigUIntBinOp(masked, awst::BigUIntBinaryOperator::Add,
			awst::makeBiguintConstant(offset.str(), _loc), _loc),
		masked, awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendToUint64(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	if (_bits == 0 || _bits >= 64) return _value;
	// The source (including any effects) is evaluated once. Masking also
	// handles input already sign-extended by an earlier ABI decode.
	auto masked = awst::makeEvalOnce(awst::makeUInt64BinOp(std::move(_value),
		awst::UInt64BinaryOperator::BitAnd,
		awst::makeIntegerConstant((uint64_t{1} << _bits) - 1, _loc), _loc), _loc);
	return awst::makeConditional(awst::makeNumericCompare(
		masked, awst::NumericComparison::Gte,
		awst::makeIntegerConstant(uint64_t{1} << (_bits - 1), _loc), _loc),
		awst::makeUInt64BinOp(masked, awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(~((uint64_t{1} << _bits) - 1), _loc), _loc),
		masked, awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::maskUnsignedToWidth(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	// Canonicalise an unsigned sub-256 biguint to its width: `value mod 2^bits`. Callers guard
	// `bits < 256` (the 256-bit case is a no-op and `u256(1) << 256` overflows). The dual of
	// signExtendToUint256 for the unsigned side; the invariant the v427–v432 fixes share.
	return awst::makeBigUIntBinOp(
		std::move(_value), awst::BigUIntBinaryOperator::Mod,
		awst::makeIntegerConstant((solidity::u256(1) << _bits).str(), _loc, awst::WType::biguintType()),
		_loc);
}

std::pair<std::string, std::string> TypeCoercion::pow2NAndHalf(unsigned _bits)
{
	// 2^N (wrap modulus) and 2^(N-1) (signed sign-bit / INT_MIN boundary). u256(1) << 256
	// overflows, so the full-width case uses the centralised literals.
	if (_bits == 256)
		return {kPow2_256, kHalfMax_256};
	return {(solidity::u256(1) << _bits).str(), (solidity::u256(1) << (_bits - 1)).str()};
}

std::shared_ptr<awst::Expression> TypeCoercion::isNegativeSigned(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	// Negative iff the sign bit is set: value >= 2^(bits-1), for a canonical two's-complement
	// biguint value. pow2NAndHalf handles the bits==256 overflow case.
	auto threshold = awst::makeIntegerConstant(
		pow2NAndHalf(_bits).second, _loc, awst::WType::biguintType());
	return awst::makeNumericCompare(
		std::move(_value), awst::NumericComparison::Gte, std::move(threshold), _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::coerceToCommonInt(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _srcSol,
	awst::WType const* _commonW,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;
	// 1. wtype: uint64<->biguint (a negative biguint literal narrows to low 64-bit TC).
	auto v = implicitNumericCast(std::move(_value), _commonW, _loc);
	// 2. sign-extend a SIGNED operand from its OWN source width so the value is canonical
	//    at the common width. Unsigned operands carry no sign (zero-extend is right); a
	//    literal (RationalNumberType, not an IntegerType) is already canonical post-cast.
	if (auto srcInt = SolIntType::fromSol(_srcSol); srcInt && srcInt->isSigned)
		v = (_commonW == awst::WType::biguintType())
			? signExtendToUint256(std::move(v), srcInt->bits, _loc)
			: signExtendToUint64(std::move(v), srcInt->bits, _loc);
	return v;
}

std::shared_ptr<awst::Expression> TypeCoercion::canonicalIntConstant(
	solidity::u256 const& _tcValue,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	if (_bits <= 64)
	{
		// Low 64-bit two's complement: 0xff..ff for int8 -1, value as-is for unsigned.
		static solidity::u256 const twoPow64 = solidity::u256(1) << 64;
		solidity::u256 v = _tcValue % twoPow64;
		return awst::makeIntegerConstant(v.str(), _loc, awst::WType::uint64Type());
	}
	// >64 bits: the value is already the canonical 256-bit two's complement.
	return awst::makeIntegerConstant(_tcValue.str(), _loc, awst::WType::biguintType());
}

std::shared_ptr<awst::Expression> TypeCoercion::rationalIntConstant(
	solidity::u256 const& _value,
	awst::WType const* _mappedType,
	awst::SourceLocation const& _loc
)
{
	static solidity::u256 const uint64Max("18446744073709551615");
	awst::WType const* wtype =
		(_mappedType == awst::WType::uint64Type() && _value > uint64Max)
			? awst::WType::biguintType()
			: _mappedType;
	return awst::makeIntegerConstant(_value.str(), _loc, wtype);
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendSignedElement(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _solElemType,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;
	// Only biguint-backed signed elements (64 < N < 256) need extension. int256
	// is already canonical two's complement; <=64-bit elements are uint64-backed
	// and carry their own sign handling (a 256-bit extension would mis-type them).
	if (auto it = SolIntType::fromSol(_solElemType);
		it && it->isSigned && it->bits > 64 && it->bits < 256)
		return signExtendToUint256(std::move(_value), it->bits, _loc);
	return _value;
}

namespace
{

/// Shared body of checkedIndexToUint64 / checkedAmountToUint64: a biguint
/// value is pinned to a `<_tmpPrefix><n>` temp (n drawn from the
/// `_counterKey` NameGen sequence), asserted `< 2^64` with `_message`, and the
/// temp (or a uint64 value, untouched) narrows through implicitNumericCast.
/// The two public wrappers differ ONLY in those three strings.
std::shared_ptr<awst::Expression> checkedNarrowToUint64(
	std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	char const* _tmpPrefix,
	char const* _counterKey,
	char const* _message
)
{
	if (_value && _value->wtype == awst::WType::biguintType())
	{
		std::string nm = _tmpPrefix + std::to_string(awst::NameGen::next(_counterKey));
		_preStmts.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, awst::WType::biguintType(), _loc), std::move(_value), _loc));
		auto fits = awst::makeNumericCompare(
			awst::makeVarExpression(nm, awst::WType::biguintType(), _loc),
			awst::NumericComparison::Lt,
			awst::makeIntegerConstant("18446744073709551616", _loc, awst::WType::biguintType()), _loc);
		_preStmts.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(fits), _loc, _message), _loc));
		_value = awst::makeVarExpression(nm, awst::WType::biguintType(), _loc);
	}
	return TypeCoercion::implicitNumericCast(std::move(_value), awst::WType::uint64Type(), _loc);
}

} // namespace

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
	return checkedNarrowToUint64(_preStmts, std::move(_idx), _loc,
		"__ckidx_", "TypeCoercion.checkedIndex", "array index out of bounds");
}

std::shared_ptr<awst::Expression> TypeCoercion::checkedAmountToUint64(
	std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc
)
{
	// Monetary amount → uint64 with an overflow PRE-check: a biguint amount
	// >= 2^64 can't be represented in the AVM amount field, so assert it fits
	// BEFORE truncating — else `payable(to).transfer(100 ether)` (1e20 > 2^64)
	// silently sends 1e20 mod 2^64 microAlgos. Pin to a temp so a side-effecting
	// amount evaluates once.
	return checkedNarrowToUint64(_preStmts, std::move(_amount), _loc,
		"__ckamt_", "TypeCoercion.checkedAmount",
		"transfer amount exceeds uint64 (AVM amounts are 64-bit)");
}

std::shared_ptr<awst::Expression> TypeCoercion::checkedAllocationSizeToUint64(
	std::vector<std::shared_ptr<awst::Statement>>& pre,
	std::shared_ptr<awst::Expression> size, awst::SourceLocation const& loc)
{
	return checkedNarrowToUint64(pre, std::move(size), loc,
		"__cksize_", "TypeCoercion.checkedAllocationSize", "allocation size exceeds uint64");
}

void TypeCoercion::assertImplicitlyConvertible(
	solidity::frontend::Type const* _srcSolType,
	solidity::frontend::Type const* _tgtSolType,
	awst::SourceLocation const& _loc,
	char const* _site
)
{
	if (!_srcSolType || !_tgtSolType)
		return;
	using namespace solidity::frontend;
	// The external callable view is solc's own calldata-to-memory adapter.
	// Preserve kind, nominal arguments and mutability checks; never waive all
	// function conversions merely because the runtime handle has equal width.
	auto externalView = [](Type const* type) -> Type const* {
		auto const* function = dynamic_cast<FunctionType const*>(type);
		return function && function->kind() == FunctionType::Kind::External
			? function->asExternallyCallableFunction(false) : type;
	};
	_srcSolType = externalView(_srcSolType);
	_tgtSolType = externalView(_tgtSolType);
	// Accept when EITHER the raw pair OR the memory-normalized pair converts.
	// Raw-only: storage→storage array copies convert element types, their
	// memory forms don't. Normalized-only: internal calls to public fns with
	// CALLDATA params legally take MEMORY args. The mixup class this tripwire
	// targets (sign/width/kind) fails BOTH forms. Mapping-containing types
	// can't be re-located (storage-only) — raw check only.
	if (_srcSolType->isImplicitlyConvertibleTo(*_tgtSolType))
		return;
	if (!containsMappingType(_srcSolType) && !containsMappingType(_tgtSolType))
	{
		auto valueLocation = [](Type const* type) -> Type const* {
			// ArraySliceType explicitly forbids copyForLocation(). Its mobile
			// type is relocatable only where solc supplies an underlying array.
			if (dynamic_cast<ArraySliceType const*>(type)) type = type->mobileType();
			return dynamic_cast<ArrayType const*>(type) || dynamic_cast<StructType const*>(type)
				? TypeProvider::withLocationIfReference(DataLocation::Memory, type) : type;
		};
		_srcSolType = valueLocation(_srcSolType);
		_tgtSolType = valueLocation(_tgtSolType);
	}
	if (!_srcSolType->isImplicitlyConvertibleTo(*_tgtSolType))
		Logger::instance().error(
			std::string("internal type-plumbing error at ") + _site
				+ ": lowering an implicit conversion solc says is ILLEGAL ("
				+ _srcSolType->humanReadableName() + " -> "
				+ _tgtSolType->humanReadableName()
				+ "). The source type-checked, so the compiler picked the wrong "
				  "source/target types here — report this.",
			_loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::signExtendSignedWiden(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _srcSolType,
	solidity::frontend::Type const* _tgtSolType,
	awst::SourceLocation const& _loc
)
{
	// Widening a SIGNED intN to a wider SIGNED intM drops the sign in our value model: sub-word
	// ints are uint64-backed (so int8->int16 is a uint64->uint64 no-op) and the registry/cast
	// zero-extends. Re-extend from the SOURCE width. Covers both target tiers (≤64 uint64-backed,
	// >64 biguint-backed). No-op for unsigned, narrowing, same-width, or non-int operands.
	auto srcInt = SolIntType::fromSol(_srcSolType);
	auto tgtInt = SolIntType::fromSol(_tgtSolType);
	if (!_value || !srcInt || !tgtInt) return _value;
	if (!srcInt->isSigned || !tgtInt->isSigned) return _value;
	if (srcInt->bits >= tgtInt->bits) return _value;
	assertImplicitlyConvertible(_srcSolType, _tgtSolType, _loc, "signExtendSignedWiden");
	if (tgtInt->bits > 64 && _value->wtype == awst::WType::biguintType())
		return signExtendToUint256(std::move(_value), srcInt->bits, _loc);
	if (_value->wtype == awst::WType::uint64Type())
		return signExtendToUint64(std::move(_value), srcInt->bits, _loc);
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
	// Match Puya's alias-aware name of the actual emitted wire type. Native
	// biguint is uint512 there; Solidity boundaries explicitly plan uintN.
	if (!_type || _type == awst::WType::voidType()) return "void";
	if (_type == awst::WType::boolType() || _type == awst::WType::arc4BoolType()) return "bool";
	if (_type == awst::WType::uint64Type() || _type == awst::WType::applicationType()) return "uint64";
	if (_type == awst::WType::biguintType()) return "uint512";
	if (_type == awst::WType::accountType()) return "address";
	if (_type == awst::WType::stringType()) return "string";
	if (auto length = awst::fixedBytesLength(_type))
		return "byte[" + std::to_string(*length) + "]";
	if (_type->kind() == awst::WTypeKind::Bytes) return "byte[]";
	if (auto const* integer = dynamic_cast<awst::ARC4UIntN const*>(_type))
		return integer->arc4Alias().empty()
			? "uint" + std::to_string(integer->n()) : integer->arc4Alias();
	if (auto const* fixed = dynamic_cast<awst::ARC4UFixedNxM const*>(_type))
		return "ufixed" + std::to_string(fixed->n()) + "x" + std::to_string(fixed->m());
	if (auto const* array = dynamic_cast<awst::ARC4StaticArray const*>(_type))
		return array->arc4Alias().empty()
			? wtypeToABIName(array->elementType()) + "[" + std::to_string(array->arraySize()) + "]"
			: array->arc4Alias();
	if (auto const* array = dynamic_cast<awst::ARC4DynamicArray const*>(_type))
		return array->arc4Alias().empty()
			? wtypeToABIName(array->elementType()) + "[]" : array->arc4Alias();

	std::vector<awst::WType const*> fields;
	if (auto const* structure = dynamic_cast<awst::ARC4Struct const*>(_type))
		for (auto const& [name, type]: structure->fields()) fields.push_back(type);
	else if (auto const* tuple = dynamic_cast<awst::ARC4Tuple const*>(_type))
		fields = tuple->types();
	else if (auto const* tuple = dynamic_cast<awst::WTuple const*>(_type))
		fields = tuple->types();
	else
		throw SizeError("type has no ARC4 wire signature: " + _type->name());
	std::string result = "(";
	for (size_t i = 0; i < fields.size(); ++i)
	{
		if (i) result += ",";
		result += wtypeToABIName(fields[i]);
	}
	return result + ")";
}

std::string TypeCoercion::buildArc4Selector(
	std::string const& _name,
	std::vector<std::string> const& _paramNames,
	std::vector<std::string> const& _retNames)
{
	auto join = [](std::vector<std::string> const& _parts) {
		std::string s;
		for (size_t i = 0; i < _parts.size(); ++i)
		{
			if (i) s += ",";
			s += _parts[i];
		}
		return s;
	};
	std::string sel = _name + "(" + join(_paramNames) + ")";
	if (_retNames.size() > 1)
		sel += "(" + join(_retNames) + ")";
	else if (_retNames.size() == 1)
		sel += _retNames[0];
	else
		sel += "void";
	return sel;
}

// ── Defaults ─────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> TypeCoercion::relabelUnsizedBytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	if (!_expr || !_expr->wtype || !_targetType)
		return _expr;
	if (awst::structurallyEquivalent(_expr->wtype, _targetType))
		return _expr;
	auto const* dst = dynamic_cast<awst::BytesWType const*>(_targetType);
	auto const* src = dynamic_cast<awst::BytesWType const*>(_expr->wtype);
	if (!dst || !src || !dst->length().has_value() || src->length().has_value())
		return _expr;
	return awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
}

std::shared_ptr<awst::Expression> TypeCoercion::makeDefaultValue(
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	if (!_type)
		return awst::makeBytesConstant({}, _loc);
	if (_type == awst::WType::boolType())
		return awst::makeFalse(_loc);
	if (_type == awst::WType::uint64Type())
		return awst::makeZero(_loc);
	if (_type == awst::WType::biguintType())
		return awst::makeBiguintConstant("0", _loc);

	if (isArc4EncodedType(_type))
	{
		// One layout/default implementation owns packed bool runs and dynamic
		// head/tail offsets. Large fixed zeros use runtime creation; a failed
		// dynamic encoding is an error, never an empty or guessed value.
		auto const size = computeEncodedElementSize(_type).fixedBytes<int>();
		if (size && *size > kLargeBytesRuntimeThreshold)
			return makeZeroBytesRuntime(*size, _type, _loc);
		if (auto bytes = arc4DefaultEncoding(_type))
			return awst::makeBytesConstant(
				std::move(*bytes), _loc, awst::BytesEncoding::Base16, _type);
		throw SizeError("ARC4 default encoding is unsupported or exceeds the materialization limit");
	}

	if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(_type))
	{
		auto tuple = awst::makeTupleExpression(_type, _loc);
		for (auto const* component: tupleType->types())
			tuple->items.push_back(makeDefaultValue(component, _loc));
		return tuple;
	}
	if (auto const* arrayType = dynamic_cast<awst::ReferenceArray const*>(_type))
	{
		auto array = awst::makeNewArray(_type, _loc);
		for (int64_t i = 0; i < arrayType->arraySize().value_or(0); ++i)
			array->values.push_back(makeDefaultValue(arrayType->elementType(), _loc));
		return array;
	}

	std::vector<uint8_t> bytes;
	if (_type == awst::WType::accountType())
		bytes.assign(32, 0);
	else if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type);
		bytesType && bytesType->length())
	{
		int const size = computeEncodedElementSize(_type).fixedBytes<int>().value();
		if (size > kLargeBytesRuntimeThreshold)
			return makeZeroBytesRuntime(size, _type, _loc);
		bytes.assign(static_cast<size_t>(size), 0);
	}
	return awst::makeBytesConstant(
		std::move(bytes), _loc, awst::BytesEncoding::Base16, _type);
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

namespace
{

// ── coerceForAssignment rungs, tried in the order below. Each returns nullptr
// when its shape doesn't apply and only then leaves `_expr` untouched; a claim
// consumes `_expr` and yields the converted value. ────────────────────────

/// Scalar ARC4 integer widening (arc4.uintN → arc4.uintM, arc4.intN →
/// arc4.intM, N < M, same signedness — the only integer element conversions
/// solc admits implicitly). Both are byte-level: unsigned prepends zeros,
/// signed prepends a runtime 0x00/0xFF run keyed on the top bit. A general
/// rule here means every aggregate copy that recurses per element inherits
/// it — mirroring solc, where array copies delegate to the one scalar
/// conversion function instead of enumerating element kinds.
std::shared_ptr<awst::Expression> tryWidenArc4ScalarInt(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	auto const* srcInt = dynamic_cast<awst::ARC4UIntN const*>(_expr->wtype);
	auto const* tgtInt = dynamic_cast<awst::ARC4UIntN const*>(_targetType);
	if (!(srcInt && tgtInt && srcInt->n() < tgtInt->n()
		&& srcInt->n() % 8 == 0 && tgtInt->n() % 8 == 0
		&& srcInt->isSigned() == tgtInt->isSigned()))
		return nullptr;
	int const pad = static_cast<int>((tgtInt->n() - srcInt->n()) / 8);
	if (!srcInt->isSigned())
		return awst::makeReinterpretCast(
			awst::makeLeftPad(
				awst::makeAsBytes(std::move(_expr), _loc), pad, _loc),
			_targetType, _loc);
	auto once = awst::makeEvalOnce(
		awst::makeAsBytes(std::move(_expr), _loc), _loc);
	auto signByte = awst::makeBtoi(
		awst::makeExtract(once, 0, 1, _loc), _loc);
	auto isNeg = awst::makeNumericCompare(
		std::move(signByte), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(128, _loc), _loc);
	auto prefix = awst::makeConditional(
		std::move(isNeg),
		awst::makeBytesConstant(
			std::vector<uint8_t>(static_cast<size_t>(pad), 0xFFu), _loc),
		awst::makeBzero(pad, _loc),
		awst::WType::bytesType(), _loc);
	return awst::makeReinterpretCast(
		awst::makeConcat(std::move(prefix), once, _loc), _targetType, _loc);
}

/// String/bytes source → fixed-size bytes[N] target of a DIFFERENT width.
/// For fixed-size bytes[N] targets coming from a narrower fixed bytes[M]
/// (M < N), Solidity right-pads the source with zeros to produce N bytes. A
/// bare ReinterpretCast leaves the source's M bytes labelled as bytes[N],
/// which decodes to the wrong width at the call boundary; build the padded
/// value explicitly. nullptr = unsized target, unknown source width, or
/// widths already equal (the caller's plain ReinterpretCast applies).
std::shared_ptr<awst::Expression> tryResizeFixedBytes(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	auto const* tw = dynamic_cast<awst::BytesWType const*>(_targetType);
	if (!tw || !tw->length().has_value())
		return nullptr;
	int targetWidth = static_cast<int>(*tw->length());
	int sourceWidth = 0;
	if (auto const* sw = dynamic_cast<awst::BytesWType const*>(_expr->wtype))
		if (sw->length().has_value())
			sourceWidth = static_cast<int>(*sw->length());
	// Hex/string literals can carry the generic bytes representation
	// even though their concrete byte count is known. Preserve that
	// width for the same fixed-bytes conversion used by assignment,
	// return, initialization, and call arguments.
	if (sourceWidth == 0)
		if (auto const* bytes = dynamic_cast<awst::BytesConstant const*>(
			_expr.get()))
			sourceWidth = static_cast<int>(bytes->value.size());
	if (!(sourceWidth > 0 && sourceWidth != targetWidth))
		return nullptr;
	auto srcBytes = awst::makeAsBytes(std::move(_expr), _loc);
	std::shared_ptr<awst::Expression> resized;
	if (sourceWidth < targetWidth)
		resized = awst::makeRightPad(
			std::move(srcBytes), targetWidth - sourceWidth, _loc);
	else
		resized = awst::makeExtract3(
			std::move(srcBytes), awst::makeZero(_loc),
			awst::makeIntegerConstant(targetWidth, _loc), _loc);
	return awst::makeReinterpretCast(
		std::move(resized), _targetType, _loc);
}

/// Bytes-kind target: IntegerConstant → BytesConstant(bytes[N]), string
/// literal → right-padded bytes[N], then any string/bytes-compatible source
/// via (width-adjusting) ReinterpretCast. nullptr = not a bytes target, or a
/// source that is none of those (e.g. an account — the next rung's shape).
std::shared_ptr<awst::Expression> tryCoerceToBytes(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	// String storage and byte views use the same bytes on AVM, but retain
	// distinct AWST types. This belongs at every conversion boundary, not
	// only in the former scalar-assignment fallback.
	if (_targetType == awst::WType::stringType()
		&& _expr->wtype->kind() == awst::WTypeKind::Bytes)
		return awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
	if (_targetType->kind() != awst::WTypeKind::Bytes)
		return nullptr;
	auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_targetType);
	if (bytesType && bytesType->length().has_value())
	{
		int N = static_cast<int>(*bytesType->length());

		// IntegerConstant → bytes[N]
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
		{
			return awst::makeBytesConstant(
				TypeCoercion::intLiteralToBytesN(intConst->value, N), _loc,
				awst::BytesEncoding::Base16, _targetType);
		}

		// String → bytes[N] (right-padded)
		if (auto padded = TypeCoercion::stringToBytesN(_expr.get(), _targetType, N, _loc))
			return padded;
	}

	// String/bytes-compatible → bytes via ReinterpretCast.
	if (_expr->wtype == awst::WType::stringType()
		|| _expr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		if (auto resized = tryResizeFixedBytes(_expr, _targetType, _loc))
			return resized;
		auto cast = awst::makeReinterpretCast(std::move(_expr), _targetType, _loc);
		return cast;
	}
	return nullptr;
}

/// Account ↔ bytes[32]: a relabelling ReinterpretCast either way.
std::shared_ptr<awst::Expression> tryAccountBytesReinterpret(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
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
	return nullptr;
}

/// Physical account → application coercion accepts only the canonical encoding.
/// Profile-aware runtime self aliases belong to ApplicationTarget::resolve.
std::shared_ptr<awst::Expression> tryAccountToApplication(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (!(_targetType == awst::WType::applicationType()
		&& _expr->wtype == awst::WType::accountType()))
		return nullptr;
	return ApplicationTarget::requireApplication(
		ApplicationTarget::canonicalId(std::move(_expr), _loc), _loc);
}

/// uint64 → bool (0/non-0)
std::shared_ptr<awst::Expression> tryUInt64ToBool(
	std::shared_ptr<awst::Expression>& _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (!(_targetType == awst::WType::boolType()
		&& _expr->wtype == awst::WType::uint64Type()))
		return nullptr;
	auto zero = awst::makeZero(_loc);
	auto cmp = awst::makeNumericCompare(std::move(_expr), awst::NumericComparison::Ne, std::move(zero), _loc);
	return cmp;
}

} // namespace

std::shared_ptr<awst::Expression> TypeCoercion::coerceForAssignment(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>* _pre)
{
	if (!_expr || !_targetType || _expr->wtype == _targetType)
		return _expr;

	// Numeric cast (uint64 ↔ biguint)
	_expr = implicitNumericCast(std::move(_expr), _targetType, _loc);
	if (_expr->wtype == _targetType)
		return _expr;

	// Rungs in shape order; the first to claim `_expr` decides. The scalar
	// ARC4 integer widening comes first so every aggregate copy that recurses
	// per element inherits it.
	if (auto widened = tryWidenArc4ScalarInt(_expr, _targetType, _loc))
		return widened;
	// One array emitter owns shape, element conversion, copying and padding.
	if (auto copied = tryConvertArc4Array(_expr, _targetType, _pre, _loc))
		return copied;
	if (auto bytes = tryCoerceToBytes(_expr, _targetType, _loc))
		return bytes;
	if (auto cast = tryAccountBytesReinterpret(_expr, _targetType, _loc))
		return cast;
	if (auto cast = tryAccountToApplication(_expr, _targetType, _loc))
		return cast;
	if (auto cmp = tryUInt64ToBool(_expr, _targetType, _loc))
		return cmp;

	return _expr;
}

} // namespace puyasol::builder
