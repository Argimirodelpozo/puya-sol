/// @file SlotWordCodec.cpp
/// See SlotWordCodec.h. The transforms here are the single source of truth for
/// "what bytes does this value occupy in an EVM slot" — StorageDispatch (typed
/// app-global cells) and slot-handle element access both delegate here.

#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "Logger.h"

namespace puyasol::builder
{

namespace
{
/// bytesN mapped as arc4 byte[N]: a static array of 1-byte uints whose byte
/// backing is exactly the raw N bytes.
bool isByteArray(awst::WType const* _w, unsigned _size)
{
	auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_w);
	if (!sa)
		return false;
	auto const* elem = dynamic_cast<awst::ARC4UIntN const*>(sa->elementType());
	return elem && elem->n() == 8 && sa->arraySize() == static_cast<int64_t>(_size);
}
} // namespace

std::shared_ptr<awst::Expression> SlotWordCodec::nativeToPackedBytes(
	std::shared_ptr<awst::Expression> _value,
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
		return nativeToPackedBytes(std::move(asU64),
			awst::WType::uint64Type(), _size, _loc);
	}
	if (_wtype == awst::WType::biguintType())
	{
		// Canonical 256-bit TC (signed) / plain magnitude (unsigned): the
		// trailing `size` bytes of the 32-byte form are the packed content.
		auto padded = awst::makeZeroExtendToN(awst::makeAsBytes(std::move(_value), _loc), 32, _loc);
		return awst::makeExtract(std::move(padded),
			static_cast<int>(32 - _size), static_cast<int>(_size), _loc);
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
		auto u64 = awst::makeConditional(std::move(decoded),
			awst::makeIntegerConstant("1", _loc), awst::makeIntegerConstant("0", _loc),
			awst::WType::uint64Type(), _loc);
		return awst::makeExtract(awst::makeItob(std::move(u64), _loc),
			static_cast<int>(8 - _size), static_cast<int>(_size), _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::ARC4UIntN)
	{
		// arc4.uintN backing = exactly N/8 big-endian bytes; pad/trim to `size`.
		auto bytesView = awst::makeAsBytes(std::move(_value), _loc);
		auto padded = awst::makeZeroExtendToN(std::move(bytesView), 32, _loc);
		return awst::makeExtract(std::move(padded),
			static_cast<int>(32 - _size), static_cast<int>(_size), _loc);
	}
	if (auto const* bw = dynamic_cast<awst::BytesWType const*>(_wtype);
		bw && bw->length().has_value()
		&& static_cast<unsigned>(*bw->length()) < _size)
	{
		// byte[K] value in a WIDER window (external fn-ptr byte[12] inside
		// solc's 24-byte external-function share): LEFT-aligned, trailing
		// zeros — the convention every read/write arm here shares.
		return awst::makeConcat(
			awst::makeAsBytes(std::move(_value), _loc),
			awst::makeBzero(static_cast<int>(
				_size - static_cast<unsigned>(*bw->length())), _loc), _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::Bytes)
		return awst::makeAsBytes(std::move(_value), _loc);   // bytes[N]: raw N bytes
	if (isByteArray(_wtype, _size))
		return awst::makeAsBytes(std::move(_value), _loc);   // arc4 byte[N]: raw N bytes
	if (auto const* nb = dynamic_cast<awst::ARC4StaticArray const*>(_wtype);
		nb && nb->arraySize() > 0
		&& static_cast<unsigned>(nb->arraySize()) < _size
		&& [&]{ auto const* e = dynamic_cast<awst::ARC4UIntN const*>(
			nb->elementType()); return e && e->n() == 8; }())
	{
		// byte[K] value in a WIDER window: LEFT-aligned, trailing zeros
		// (matches the BytesWType arm — one convention for both labels of
		// the same fn-ptr handle).
		return awst::makeConcat(
			awst::makeAsBytes(std::move(_value), _loc),
			awst::makeBzero(static_cast<int>(
				_size - static_cast<unsigned>(nb->arraySize())), _loc), _loc);
	}
	if (_wtype && _wtype->name() == "address"
		&& dynamic_cast<awst::ARC4StaticArray const*>(_wtype) && _size <= 32)
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

	Logger::instance().error(
		"unsupported type '" + std::string(_wtype ? _wtype->name() : "<null>")
		+ "' in packed storage slot", _loc);
	return awst::makeBytesConstant(std::vector<uint8_t>(_size, 0), _loc);
}

std::shared_ptr<awst::Expression> SlotWordCodec::packedBytesToNative(
	std::shared_ptr<awst::Expression> _raw,
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
		{
			uint64_t half = 1ULL << (it->bits - 1);
			uint64_t addend = ~((1ULL << it->bits) - 1);
			auto isNeg = awst::makeNumericCompare(u64, awst::NumericComparison::Gte,
				awst::makeIntegerConstant(half, _loc), _loc);
			auto extended = awst::makeUInt64BinOp(u64, awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(addend, _loc), _loc);
			u64 = awst::makeConditional(std::move(isNeg), std::move(extended), u64,
				awst::WType::uint64Type(), _loc);
		}
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
		auto u64 = packedBytesToNative(std::move(_raw),
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
	if (auto const* bw = dynamic_cast<awst::BytesWType const*>(_wtype);
		bw && bw->length().has_value()
		&& static_cast<unsigned>(*bw->length()) < _size)
		return awst::makeReinterpretCast(
			awst::makeExtract(std::move(_raw), 0,
				static_cast<int>(*bw->length()), _loc), _wtype, _loc);
	if (_wtype && _wtype->kind() == awst::WTypeKind::Bytes)
		return awst::makeReinterpretCast(std::move(_raw), _wtype, _loc);
	if (isByteArray(_wtype, _size))
		return awst::makeReinterpretCast(std::move(_raw), _wtype, _loc);
	if (auto const* nb = dynamic_cast<awst::ARC4StaticArray const*>(_wtype);
		nb && nb->arraySize() > 0
		&& static_cast<unsigned>(nb->arraySize()) < _size
		&& [&]{ auto const* e = dynamic_cast<awst::ARC4UIntN const*>(
			nb->elementType()); return e && e->n() == 8; }())
	{
		unsigned k = static_cast<unsigned>(nb->arraySize());
		return awst::makeReinterpretCast(
			awst::makeExtract(std::move(_raw), 0, static_cast<int>(k), _loc),
			_wtype, _loc);
	}
	if (_wtype && _wtype->name() == "address"
		&& dynamic_cast<awst::ARC4StaticArray const*>(_wtype) && _size <= 32)
		return awst::makeReinterpretCast(
			awst::makeLeftPad(std::move(_raw), 32 - _size, _loc), _wtype, _loc);

	Logger::instance().error(
		"unsupported type '" + std::string(_wtype ? _wtype->name() : "<null>")
		+ "' in packed storage slot", _loc);
	return nullptr;
}

} // namespace puyasol::builder
