/// @file SlotWordCodec.cpp
/// See SlotWordCodec.h. The transforms here are the single source of truth for
/// "what bytes does this value occupy in an EVM slot" — StorageDispatch (typed
/// app-global cells) and slot-handle element access both delegate here.

#include "builder/codec/SlotWordCodec.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "Logger.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

namespace
{
/// Byte-shaped values are left-aligned, unlike ARC4's address alias.
std::optional<int64_t> byteWidth(awst::WType const* _w)
{
	if (_w && _w->name() == "address") return std::nullopt;
	if (auto width = awst::fixedBytesLength(_w)) return *width;
	if (auto const* array = dynamic_cast<awst::ARC4StaticArray const*>(_w);
		array && array->arraySize() > 0)
		if (auto const* elem = dynamic_cast<awst::ARC4UIntN const*>(array->elementType());
			elem && elem->n() == 8) return array->arraySize();
	return std::nullopt;
}

/// arc4.address (byte[32] alias) packed into a <=32-byte window.
bool isArc4Address(awst::WType const* _w, unsigned _size)
{
	return _w && _w->name() == "address"
		&& dynamic_cast<awst::ARC4StaticArray const*>(_w) && _size <= 32;
}
} // namespace

namespace
{

/// Scalar / word-convention arms of nativeToPackedBytes (uint64, bool, application, biguint, account, arc4 bool, arc4.uintN).
std::shared_ptr<awst::Expression> tryPackScalarWord(
	std::shared_ptr<awst::Expression>& _value,
	awst::WType const* _wtype,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (_wtype == awst::WType::uint64Type() || _wtype == awst::WType::boolType())
	{
		// uint64-backed (incl. sub-64 signed: cell holds 64-bit TC, whose low
		// `size` bytes ARE the packed TC). bool → 0/1.
		std::shared_ptr<awst::Expression> u64 = std::move(_value);
		if (_wtype == awst::WType::boolType())
			u64 = awst::makeConditional(std::move(u64),
				awst::makeIntegerConstant("1", _loc), awst::makeIntegerConstant("0", _loc),
				awst::WType::uint64Type(), _loc);
		auto itob = awst::makeItob(std::move(u64), _loc);
		if (_size == 8)
			return itob;
		if (_size > 8)
			return awst::makeLeftPad(std::move(itob), _size - 8, _loc);
		return awst::makeExtract(std::move(itob),
			static_cast<int>(8 - _size), static_cast<int>(_size), _loc);
	}
	if (_wtype == awst::WType::applicationType())
	{
		// Contract references (a `C c;` state var, `new Child()` stored in the
		// ctor — 15 slot-lane fixtures) are uint64 app ids at heart; store the
		// id like a uint64. asBytes(application) is not a cast puya has, which
		// is exactly the backend crash this arm removes.
		auto asU64 = awst::makeReinterpretCast(
			std::move(_value), awst::WType::uint64Type(), _loc);
		return SlotWordCodec::nativeToPackedBytes(std::move(asU64),
			awst::WType::uint64Type(), _size, _loc);
	}
	if (_wtype == awst::WType::biguintType())
	{
		// Canonical 256-bit TC (signed) / plain magnitude (unsigned): the
		// trailing `size` bytes of the 32-byte form are the packed content.
		auto padded = awst::makeLeftPadToN(awst::makeAsBytes(std::move(_value), _loc), _size, _loc);
		return awst::makeExtractLastN(std::move(padded), static_cast<int>(_size), _loc);
	}
	if (_wtype == awst::WType::accountType())
	{
		// AVM account = 32 bytes; EVM address = trailing 20 (transient-codec convention).
		return awst::makeExtract(awst::makeAsBytes(std::move(_value), _loc),
			static_cast<int>(32 - _size), static_cast<int>(_size), _loc);
	}
	if (_wtype == awst::WType::arc4BoolType())
	{
		// ARC4 bool encodes true as 0x80; the packed slot byte is canonical 0x01.
		auto decoded = awst::makeARC4Decode(std::move(_value), awst::WType::boolType(), _loc);
		return SlotWordCodec::nativeToPackedBytes(std::move(decoded),
			awst::WType::boolType(), _size, _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::ARC4UIntN)
	{
		// arc4.uintN backing = exactly N/8 big-endian bytes; pad/trim to `size`.
		auto bytesView = awst::makeAsBytes(std::move(_value), _loc);
		auto padded = awst::makeZeroExtendToN(std::move(bytesView), 32, _loc);
		return awst::makeExtract(std::move(padded),
			static_cast<int>(32 - _size), static_cast<int>(_size), _loc);
	}
	return nullptr;
}

/// Byte-shaped arms of nativeToPackedBytes (bytes[N], arc4 byte arrays, arc4.address) — the LEFT-aligned / trailing-truncation …
std::shared_ptr<awst::Expression> tryPackByteShaped(
	std::shared_ptr<awst::Expression>& _value,
	awst::WType const* _wtype,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (auto width = byteWidth(_wtype); width && *width < _size)
		return awst::makeConcat(awst::makeAsBytes(std::move(_value), _loc),
			awst::makeBzero(static_cast<int>(_size - *width), _loc), _loc);
	if ((_wtype && _wtype->kind() == awst::WTypeKind::Bytes) || byteWidth(_wtype) == _size)
		return awst::makeAsBytes(std::move(_value), _loc);
	if (isArc4Address(_wtype, _size))
	{
		// arc4.address (byte[32] alias) in a PACKED slot: the EVM packs an
		// address as its 20 bytes, and this mode's convention stores the
		// TRAILING 20 of the 32-byte AVM form (same truncation the slot
		// readers already fold — staup `_owner`). Blocked CoW's EthFlowOrder
		// and Compound's RewardConfig, both of which pack {address, small
		// ints} into one word.
		return awst::makeExtract(awst::makeAsBytes(std::move(_value), _loc),
			static_cast<int>(32 - _size), static_cast<int>(_size), _loc);
	}
	return nullptr;
}

} // namespace

bool SlotWordCodec::isByteShaped(awst::WType const* _wtype)
{
	return byteWidth(_wtype).has_value();
}

bool SlotWordCodec::supportsField(awst::WType const* type,
	solidity::frontend::Type const* solType, unsigned size)
{
	if (!type || !solType || !solType->isValueType() || size == 0 || size > 32
		|| solType->storageBytes() != size) return false;
	if (type == awst::WType::uint64Type() || type == awst::WType::boolType()
		|| type == awst::WType::biguintType() || type == awst::WType::accountType()
		|| type == awst::WType::arc4BoolType()) return true;
	return type->kind() == awst::WTypeKind::ARC4UIntN
		|| type->kind() == awst::WTypeKind::Bytes || byteWidth(type) == size
		|| isArc4Address(type, size);
}

std::shared_ptr<awst::Expression> SlotWordCodec::nativeToPackedBytes(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _wtype,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (auto packed = tryPackScalarWord(_value, _wtype, _size, _loc))
		return packed;
	if (auto packed = tryPackByteShaped(_value, _wtype, _size, _loc))
		return packed;

	Logger::instance().error(
		"unsupported type '" + std::string(_wtype ? _wtype->name() : "<null>")
		+ "' in packed storage slot", _loc);
	return awst::makeBytesConstant(std::vector<uint8_t>(_size, 0), _loc);
}

namespace
{

/// Scalar / word-convention arms of packedBytesToNative — mirrors tryPackScalarWord (unpack rung order kept verbatim: uint64/bool, …
std::shared_ptr<awst::Expression> tryUnpackScalarWord(
	std::shared_ptr<awst::Expression>& _raw,
	awst::WType const* _wtype,
	solidity::frontend::Type const* _solType,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (_wtype == awst::WType::uint64Type() || _wtype == awst::WType::boolType())
	{
		std::shared_ptr<awst::Expression> u64;
		if (_size > 8)   // e.g. contract type packed as 20 bytes: numeric low 8
			u64 = awst::makeBtoi(awst::makeExtract(std::move(_raw),
				static_cast<int>(_size - 8), 8, _loc), _loc);
		else
			u64 = awst::makeBtoi(std::move(_raw), _loc);
		// Sub-64 signed: cell convention is 64-bit TC — sign-extend from `size` bytes.
		if (auto it = SolIntType::fromSol(_solType);
			it && it->isSigned && it->bits < 64 && _wtype == awst::WType::uint64Type())
			u64 = TypeCoercion::signExtendToUint64(std::move(u64), it->bits, _loc);
		if (_wtype == awst::WType::boolType())
			return awst::makeNumericCompare(std::move(u64), awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", _loc), _loc);
		return u64;
	}
	if (_wtype == awst::WType::biguintType())
	{
		auto native = awst::makeAsBiguint(std::move(_raw), _loc);
		// 64 < bits < 256 signed: extend to the canonical 256-bit TC cell form.
		return TypeCoercion::signExtendSignedElement(std::move(native), _solType, _loc);
	}
	if (_wtype == awst::WType::accountType())
		return awst::makeAsAccount(awst::makeLeftPad(std::move(_raw), 32 - _size, _loc), _loc);
	if (_wtype == awst::WType::applicationType())
	{
		// mirror of the encode arm: low 8 bytes hold the uint64 app id
		auto u64 = SlotWordCodec::packedBytesToNative(std::move(_raw),
			awst::WType::uint64Type(), nullptr, _size, _loc);
		return awst::makeReinterpretCast(std::move(u64),
			awst::WType::applicationType(), _loc);
	}
	if (_wtype == awst::WType::arc4BoolType())
	{
		auto asBool = awst::makeNumericCompare(awst::makeBtoi(std::move(_raw), _loc),
			awst::NumericComparison::Ne, awst::makeIntegerConstant("0", _loc), _loc);
		return awst::makeARC4Encode(std::move(asBool), awst::WType::arc4BoolType(), _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::ARC4UIntN)
	{
		// arc4.uintN backing = exactly N/8 BE bytes; re-align from packed `size`.
		auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(_wtype);
		unsigned backing = uintN ? uintN->n() / 8 : _size;
		std::shared_ptr<awst::Expression> b = std::move(_raw);
		if (backing > _size)
			b = awst::makeLeftPad(std::move(b), backing - _size, _loc);
		else if (backing < _size)
			b = awst::makeExtract(std::move(b),
				static_cast<int>(_size - backing), static_cast<int>(backing), _loc);
		return awst::makeReinterpretCast(std::move(b), _wtype, _loc);
	}
	return nullptr;
}

/// Byte-shaped arms of packedBytesToNative — mirrors tryPackByteShaped.
std::shared_ptr<awst::Expression> tryUnpackByteShaped(
	std::shared_ptr<awst::Expression>& _raw,
	awst::WType const* _wtype,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (auto width = byteWidth(_wtype); width && *width < _size)
		return awst::makeReinterpretCast(awst::makeExtract(std::move(_raw),
			0, static_cast<int>(*width), _loc), _wtype, _loc);
	if ((_wtype && _wtype->kind() == awst::WTypeKind::Bytes) || byteWidth(_wtype) == _size)
		return awst::makeReinterpretCast(std::move(_raw), _wtype, _loc);
	if (isArc4Address(_wtype, _size))
		return awst::makeReinterpretCast(
			awst::makeLeftPad(std::move(_raw), 32 - _size, _loc), _wtype, _loc);
	return nullptr;
}

} // namespace

std::shared_ptr<awst::Expression> SlotWordCodec::packedBytesToNative(
	std::shared_ptr<awst::Expression> _raw,
	awst::WType const* _wtype,
	solidity::frontend::Type const* _solType,
	unsigned _size,
	awst::SourceLocation const& _loc)
{
	if (auto native = tryUnpackScalarWord(_raw, _wtype, _solType, _size, _loc))
		return native;
	if (auto native = tryUnpackByteShaped(_raw, _wtype, _size, _loc))
		return native;

	Logger::instance().error(
		"unsupported type '" + std::string(_wtype ? _wtype->name() : "<null>")
		+ "' in packed storage slot", _loc);
	return nullptr;
}

} // namespace puyasol::builder
