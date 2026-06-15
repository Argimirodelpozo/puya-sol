/// @file AbiDecode.cpp
/// ABI decode side, extracted from AbiCodecImpl.cpp:
///   - decodeAbiValue: top-level decoder driving the type-walk
///   - uint64FromAbiWord: small helper for extracting a uint64 from a
///     32-byte ABI word
#include "builder/abi/AbiEncoderBuilder.h"
#include "Logger.h"
#include "builder/abi/AbiCodecHelpers.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{
using namespace abi_codec;
}
namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AbiEncoderBuilder::uint64FromAbiWord(
	std::shared_ptr<awst::Expression> _word32,
	awst::SourceLocation const& _loc)
{
	return awst::makeWord32ToUInt64(std::move(_word32), _loc);
}

namespace
{
// EVM-ABI static byte size of a Solidity type: every value type occupies a
// 32-byte slot, a static array T[N] is N slots, a static struct is the sum
// of its fields. Returns -1 for dynamically-encoded types. Used to decide
// whether the ARC4 byte layout coincides with the EVM layout (it does iff
// ARC4 size == EVM size — i.e. every leaf is a full 32-byte word); when they
// differ (an int128 field is 16 ARC4 bytes but a 32-byte EVM slot) the slab
// reinterpret is wrong and we must walk fields.
int evmStaticSize(solidity::frontend::Type const* _t)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_t))
		_t = &udvt->underlyingType();
	if (auto const* arr = dynamic_cast<ArrayType const*>(_t))
	{
		if (arr->isByteArrayOrString() || arr->isDynamicallySized())
			return -1;
		int elem = evmStaticSize(arr->baseType());
		if (elem < 0) return -1;
		return static_cast<int>(arr->length()) * elem;
	}
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		if (_t->isDynamicallyEncoded()) return -1;
		int sum = 0;
		for (auto const& m : st->structDefinition().members())
		{
			int fs = evmStaticSize(m->type());
			if (fs < 0) return -1;
			sum += fs;
		}
		return sum;
	}
	return 32;
}
} // namespace

// ── decodeAbiValue: decode one value from EVM ABI bytes ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::decodeAbiValue(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _data,
	std::shared_ptr<awst::Expression> _offset,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// Extract the 32-byte head word at _offset
	auto headWord = awst::makeExtract3(_data, _offset, awst::makeIntegerConstant("32", _loc), _loc);
	auto* wtype = _ctx.typeMapper.map(_solType);

	// ── Static types: value is in the 32-byte head word ──

	// Integer > 64 bits → biguint (ReinterpretCast)
	if (wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeAsBiguint(std::move(headWord), _loc);
		return cast;
	}

	// Integer ≤ 64 bits → uint64 (extract last 8 bytes, btoi)
	if (wtype == awst::WType::uint64Type())
		return uint64FromAbiWord(std::move(headWord), _loc);

	// Bool → btoi != 0
	if (wtype == awst::WType::boolType())
	{
		auto val = uint64FromAbiWord(std::move(headWord), _loc);
		auto zero = awst::makeZero(_loc);
		auto cmp = awst::makeNumericCompare(std::move(val), awst::NumericComparison::Ne, std::move(zero), _loc);
		return cmp;
	}

	// Address → 32 bytes, take last 32 (it's the full word)
	if (wtype == awst::WType::accountType())
	{
		auto cast = awst::makeAsAccount(std::move(headWord), _loc);
		return cast;
	}

	// Fixed bytes (bytes1..bytes32) → left-aligned in the word, take first N
	if (auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType))
	{
		int n = static_cast<int>(fbType->numBytes());
		auto extractN = awst::makeIntrinsicCall("extract", _ctx.typeMapper.createType<awst::BytesWType>(n), _loc);
		extractN->immediates = {0, n};
		extractN->stackArgs.push_back(std::move(headWord));
		return extractN;
	}

	// ── Multi-word static types: read N contiguous bytes from _offset ──
	//
	// Static arrays / static structs / tuples whose ARC4 byte size > 32 are
	// stored inline in the EVM ABI: no offset, no length header, just
	// `total_size` bytes laid out per their structural shape. Because
	// uint256/intN/bytesN/address/contract all encode to exactly 32 bytes
	// in BOTH ABI variants, an array like `uint256[2][3]` is byte-identical
	// (6 × 32 = 192 bytes) under EVM-ABI and ARC4. Extract the slab and
	// reinterpret it as the target ARC4 type.
	//
	// Smaller-element static arrays (e.g. uint16[3]) need per-element
	// repacking and aren't covered here — fall through to the legacy
	// fallback below, which gets the wrong shape but matches prior
	// behaviour (those tests currently fail).
	if (!_solType->isDynamicallyEncoded())
	{
		auto kind = wtype->kind();
		if (kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4Struct
			|| kind == awst::WTypeKind::ARC4Tuple)
		{
			int totalSize = ::puyasol::builder::computeEncodedElementSize(wtype);
			// For an ARC4Struct the slab reinterpret is only valid when the
			// ARC4 layout coincides with EVM's — i.e. ARC4 size == EVM size, so
			// every field is a full 32-byte word. A sub-32 field (int128=16B,
			// uint8/bool=1B) makes ARC4 size SMALLER, so reinterpreting
			// `totalSize` EVM bytes reads the wrong fields; those structs go to
			// the field-walk below. (Tuples/arrays keep the original condition
			// — narrowing to the confirmed struct bug to avoid touching the
			// multi-value-decode path.)
			bool structLayoutOk = kind != awst::WTypeKind::ARC4Struct
				|| evmStaticSize(_solType) == totalSize;
			if (totalSize > 32 && structLayoutOk)
			{
				auto slab = awst::makeExtract3(
					std::move(_data),
					std::move(_offset),
					awst::makeIntegerConstant(totalSize, _loc),
					_loc);
				return awst::makeReinterpretCast(std::move(slab), wtype, _loc);
			}
		}
	}

	// ── Static struct with a sub-32 field: walk fields at EVM offsets ──
	//
	// A static struct whose ARC4 layout differs from EVM (because some field
	// is narrower than a full 32-byte word — int128=16B, uint8/bool=1B) can't
	// be slab-reinterpreted. Each static field occupies evmStaticSize bytes in
	// the EVM encoding (value types = 32), inline at `_offset`; decode each to
	// its ARC4 field type and assemble a NewStruct. (All fields are static —
	// a dynamic field would make the whole struct dynamically-encoded, handled
	// below.) This is the decode counterpart to the signed/sub-32 aggregate
	// ENCODE fix.
	if (auto const* sStructType = dynamic_cast<solidity::frontend::StructType const*>(_solType);
		sStructType && !_solType->isDynamicallyEncoded()
		&& wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* arc4Struct = static_cast<awst::ARC4Struct const*>(wtype);
		auto newStruct = awst::makeNewStruct(wtype, _loc);
		int evmOff = 0;
		for (auto const& memberDecl : sStructType->structDefinition().members())
		{
			auto const* fieldSolType = memberDecl->type();
			auto* fieldNativeType = _ctx.typeMapper.map(fieldSolType);
			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype] : arc4Struct->fields())
				if (fname == memberDecl->name()) { arc4FieldType = ftype; break; }
			if (!arc4FieldType) arc4FieldType = fieldNativeType;

			auto fieldOffset = awst::makeUInt64BinOp(
				_offset, awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(evmOff, _loc), _loc);
			auto fieldVal = decodeAbiValue(_ctx, _data, std::move(fieldOffset), fieldSolType, _loc);
			if (fieldVal->wtype != arc4FieldType && fieldNativeType != arc4FieldType)
				fieldVal = awst::makeARC4Encode(std::move(fieldVal), arc4FieldType, _loc);
			newStruct->values[memberDecl->name()] = std::move(fieldVal);

			int fs = evmStaticSize(fieldSolType);
			evmOff += (fs > 0 ? fs : 32);
		}
		return newStruct;
	}

	// ── Dynamic struct: walk fields, build NewStruct ──
	//
	// EVM ABI for a dynamic struct (when accessed by value, e.g. as a tuple
	// element or top-level abi.decode target) places a head pointer at
	// `_offset`. The struct contents start at `_offset + head_pointer`, with
	// each field occupying a 32-byte slot in the head section: static fields
	// hold the value inline, dynamic fields hold an offset (relative to the
	// struct start) to their tail. Walk each field, decode at the right
	// offset, and assemble a NewStruct.
	//
	// Limitation: head-slot assumed to be 32 bytes per field, which holds
	// for value-typed and dynamic fields. Nested static structs / multi-word
	// static arrays (>32 bytes head) fall through to the legacy fallback.
	if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_solType);
		structType && _solType->isDynamicallyEncoded()
		&& wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		using namespace solidity::frontend;
		auto const* arc4Struct = static_cast<awst::ARC4Struct const*>(wtype);
		auto const& structDef = structType->structDefinition();
		auto const& members = structDef.members();

		// All fields must fit in 32-byte head slots (covers value types and
		// any dynamic type — the head slot is either the value or an offset).
		bool allFieldsSimple = true;
		for (auto const& m : members)
		{
			auto const* ft = m->type();
			if (!ft) { allFieldsSimple = false; break; }
			if (!ft->isDynamicallyEncoded())
			{
				// Static field: must fit in one EVM head slot (32 bytes).
				auto* fwt = _ctx.typeMapper.map(ft);
				bool oneSlot = fwt == awst::WType::biguintType()
					|| fwt == awst::WType::uint64Type()
					|| fwt == awst::WType::boolType()
					|| fwt == awst::WType::accountType()
					|| fwt->kind() == awst::WTypeKind::Bytes
					|| fwt->kind() == awst::WTypeKind::ARC4UIntN;
				if (!oneSlot) { allFieldsSimple = false; break; }
			}
		}

		if (allFieldsSimple && !members.empty())
		{
			// struct_start = _offset + read_uint64(_data, _offset, 32 bytes)
			auto headOffsetWord = awst::makeExtract3(_data, _offset, awst::makeIntegerConstant("32", _loc), _loc);
			auto headOffset = uint64FromAbiWord(std::move(headOffsetWord), _loc);
			auto structStart = awst::makeUInt64BinOp(
				_offset, awst::UInt64BinaryOperator::Add, std::move(headOffset), _loc);

			auto newStruct = awst::makeNewStruct(wtype, _loc);
			for (size_t i = 0; i < members.size(); ++i)
			{
				auto const& memberDecl = members[i];
				auto const* fieldSolType = memberDecl->type();
				auto* fieldNativeType = _ctx.typeMapper.map(fieldSolType);

				// Find the ARC4 wtype for this field (if it differs from native).
				awst::WType const* arc4FieldType = nullptr;
				for (auto const& [fname, ftype]: arc4Struct->fields())
					if (fname == memberDecl->name()) { arc4FieldType = ftype; break; }
				if (!arc4FieldType) arc4FieldType = fieldNativeType;

				auto fieldHeadOffset = awst::makeUInt64BinOp(
					structStart, awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant(i * 32, _loc), _loc);

				std::shared_ptr<awst::Expression> fieldVal;
				if (!fieldSolType->isDynamicallyEncoded())
				{
					// Static field: head slot IS the value, recurse normally.
					fieldVal = decodeAbiValue(_ctx, _data, std::move(fieldHeadOffset), fieldSolType, _loc);
				}
				else
				{
					// Dynamic field: head slot holds offset relative to struct_start.
					auto fieldOffsetWordExpr = awst::makeExtract3(_data, std::move(fieldHeadOffset),
						awst::makeIntegerConstant("32", _loc), _loc);
					auto fieldOffsetWord = uint64FromAbiWord(std::move(fieldOffsetWordExpr), _loc);
					auto absoluteTail = awst::makeUInt64BinOp(
						structStart, awst::UInt64BinaryOperator::Add, std::move(fieldOffsetWord), _loc);

					// Inline the dyn-array-tail decode for the common cases:
					// 32-byte EVM element width (uint256[]/bytes32[]/address[]
					// fields) and 1-byte width (string/bytes fields — the EVM
					// tail is raw contiguous bytes, identical to the ARC4 body,
					// so only the [32-byte len] → [uint16 len] header differs).
					// Without the 1-byte case a string field fell to the
					// ARC4FromBytes fallback, which treats the first 2 DATA
					// bytes as the ARC4 header — `S(42,"hi there",7)` decoded
					// its string as " there" (silent 2-byte truncation).
					if (arc4FieldType->kind() == awst::WTypeKind::ARC4DynamicArray)
					{
						auto const* dynArr = static_cast<awst::ARC4DynamicArray const*>(arc4FieldType);
						int elemSize = ::puyasol::builder::computeEncodedElementSize(dynArr->elementType());
						if (elemSize == 32 || elemSize == 1)
						{
							// length word at absoluteTail (first 32 bytes)
							auto lenWord = awst::makeExtract3(_data, absoluteTail,
								awst::makeIntegerConstant("32", _loc), _loc);
							auto elemCount = uint64FromAbiWord(std::move(lenWord), _loc);
							auto dataStart = awst::makeUInt64BinOp(
								std::move(absoluteTail), awst::UInt64BinaryOperator::Add,
								awst::makeIntegerConstant("32", _loc), _loc);
							auto byteCount = awst::makeUInt64BinOp(
								elemCount, awst::UInt64BinaryOperator::Mult,
								awst::makeIntegerConstant(elemSize, _loc), _loc);
							auto elemBytes = awst::makeExtract3(_data, std::move(dataStart), std::move(byteCount), _loc);
							auto header = awst::makeUInt16Bytes(std::move(elemCount), _loc);
							auto arc4Bytes = awst::makeConcat(std::move(header), std::move(elemBytes), _loc);
							fieldVal = awst::makeReinterpretCast(std::move(arc4Bytes), arc4FieldType, _loc);
						}
					}
					// Unsupported dynamic field shape (dyn array with element
					// width other than 32/1, nested dynamic arrays, struct
					// elements): refuse to compile. The old ARC4FromBytes-on-
					// EVM-slab fallback handed downstream code a value whose
					// bytes are NOT the ARC4 layout its type claims — silent
					// wrong data when access happens not to trap.
					if (!fieldVal)
					{
						Logger::instance().error(
							"abi.decode of struct field type '"
							+ arc4FieldType->name()
							+ "' is not supported: the EVM tail layout for this "
							"shape has no ARC4 translation here, and a "
							"reinterpreted value would be silently wrong.", _loc);
						fieldVal = awst::makeARC4FromBytes(
							awst::makeBytesConstant({}, _loc), arc4FieldType, _loc);
					}
				}

				// If the decoded native value differs from the ARC4 field type,
				// wrap in ARC4Encode (e.g. biguint → arc4.uint256).
				if (fieldVal->wtype != arc4FieldType && fieldNativeType != arc4FieldType)
				{
					auto encoded = awst::makeARC4Encode(std::move(fieldVal), arc4FieldType, _loc);
					fieldVal = std::move(encoded);
				}
				newStruct->values[memberDecl->name()] = std::move(fieldVal);
			}
			return newStruct;
		}
	}

	// ── Dynamic types: head word contains offset to tail data ──

	if (_solType->isDynamicallyEncoded())
	{
		// The 32-byte head word is an offset relative to the start of the data
		auto tailOffset = uint64FromAbiWord(std::move(headWord), _loc);

		// At tailOffset: [length as 32 bytes][data...]
		// Read length
		auto lenWord = awst::makeExtract3(_data, tailOffset, awst::makeIntegerConstant("32", _loc), _loc);
		auto elemCount = uint64FromAbiWord(std::move(lenWord), _loc);

		// Data starts at tailOffset + 32
		auto dataStart = awst::makeUInt64BinOp(std::move(tailOffset), awst::UInt64BinaryOperator::Add, awst::makeIntegerConstant("32", _loc), _loc);

		// ARC4DynamicArray with fixed-size element: translate EVM-ABI layout
		// ([32-byte length][N × 32 bytes]) to ARC4 layout
		// ([uint16 BE length][N × elemSize bytes]). EVM pads each element to 32
		// bytes regardless of its logical size, so we bulk-copy N*elemSize bytes.
		// (Only correct when the element has a fixed ARC4 encoded size and the
		// EVM encoded size also equals 32 bytes per element, which holds for
		// uintN/intN/bool/address/bytesN but not e.g. uint256[][] or structs
		// containing dynamic fields — those keep the fallback path.)
		if (wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
		{
			auto const* dynArr = static_cast<awst::ARC4DynamicArray const*>(wtype);
			auto const* elemType = dynArr->elementType();
			int elemSize = ::puyasol::builder::computeEncodedElementSize(elemType);
			// Only safe when EVM encoded size (always 32 bytes per slot for
			// value-typed elements) matches the ARC4 encoded size — i.e., 32.
			// Covers uint256/int256/bytes32/address/contract arrays. Smaller
			// widths (uint128[], uint8[]) fall through to the generic fallback
			// because EVM slot-pads each element to 32 bytes while ARC4 packs.
			if (elemSize == 32)
			{
				// byteCount = elemCount * elemSize
				auto byteCount = awst::makeUInt64BinOp(
					elemCount,
					awst::UInt64BinaryOperator::Mult,
					awst::makeIntegerConstant(elemSize, _loc),
					_loc);

				// elemBytes = extract3(_data, dataStart, byteCount)
				auto elemBytes = awst::makeExtract3(_data, std::move(dataStart), std::move(byteCount), _loc);
				// arc4Header = extract3(itob(elemCount), 6, 2) — uint16 BE length
				auto header = awst::makeUInt16Bytes(std::move(elemCount), _loc);
				// concat(header, elemBytes)
				auto arc4Bytes = awst::makeConcat(std::move(header), std::move(elemBytes), _loc);

				// ReinterpretCast to ARC4DynamicArray<elem>
				return awst::makeReinterpretCast(std::move(arc4Bytes), wtype, _loc);
			}
		}

		// abi.decode of a dynamic array whose ELEMENTS are themselves dynamic
		// (uint256[][], bytes[], string[]) needs a recursive offset-table walk
		// the decoder doesn't implement. The bytes/string fallback below would
		// misread the element COUNT as a byte count and silently return an
		// empty/garbage array (verified: abi.decode(uint256[][]) → length 0).
		// Fail loud instead — the abi.encode side IS correct, so the round-trip
		// is one decode feature away (tracked separately). bytes/string
		// themselves have a non-dynamic (byte) element and take the path below.
		if (auto const* arrT = dynamic_cast<ArrayType const*>(_solType);
			arrT && !arrT->isByteArrayOrString()
			&& arrT->baseType() && arrT->baseType()->isDynamicallyEncoded())
		{
			Logger::instance().error(
				"abi.decode of '" + _solType->toString(true) + "' (a dynamic "
				"array with dynamic elements) is not supported: the decoder "
				"would silently return an empty array. abi.encode of this type "
				"is correct; only the nested-offset-table decode is missing.",
				_loc);
			return awst::makeARC4FromBytes(
				awst::makeBytesConstant({}, _loc), wtype, _loc);
		}

		// Extract data bytes (length word is interpreted as byte count — correct
		// for bytes/string where elements are 1 byte each)
		auto dataBytes = awst::makeExtract3(_data, std::move(dataStart), std::move(elemCount), _loc);
		// Cast to target type (string, bytes, etc.)
		if (wtype == awst::WType::stringType())
		{
			auto cast = awst::makeReinterpretCast(std::move(dataBytes), awst::WType::stringType(), _loc);
			return cast;
		}
		// ARC4-shaped targets (static arrays of dynamic elems, structs with
		// dynamic fields, tuples with dynamic elems, dynamic arrays with
		// dynamic element size): wrap the ABI-decoded bytes in ARC4FromBytes so
		// the assignment target sees a properly-typed value. The resulting
		// layout is not actually ARC4 (EVM ABI differs), so downstream access
		// will likely trap at runtime — matches the semantic-test expectation
		// of FAILURE for corrupt-input decode cases. Kept (not a hard error)
		// because those tests RELY on the runtime trap; the warning makes the
		// wrong-layout visible in the compile log. If access does NOT trap,
		// the value is silently wrong — see EVM_DIVERGENCE.md encoding seams.
		auto kind = wtype->kind();
		if (kind == awst::WTypeKind::ARC4DynamicArray
			|| kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4Struct
			|| kind == awst::WTypeKind::ARC4Tuple)
		{
			Logger::instance().warning(
				"abi.decode to '" + wtype->name()
				+ "': EVM tail layout has no direct ARC4 translation for this "
				"shape; emitting a wrong-layout value that typically traps at "
				"runtime on access.", _loc);
			return awst::makeARC4FromBytes(std::move(dataBytes), wtype, _loc);
		}
		return dataBytes;
	}

	// Fallback: ReinterpretCast the 32-byte word
	auto cast = awst::makeReinterpretCast(std::move(headWord), wtype, _loc);
	return cast;
}

// ── rightPadTo32: pad bytes to next 32-byte boundary ──

} // namespace puyasol::builder::eb
