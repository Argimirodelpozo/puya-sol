/// @file AbiDecode.cpp
/// ABI decode side, extracted from AbiCodecImpl.cpp:
///   - decodeAbiValue: top-level decoder driving the type-walk
///   - uint64FromAbiWord: small helper for extracting a uint64 from a
///     32-byte ABI word
#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-eb/AbiCodecHelpers.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

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
		auto cast = awst::makeReinterpretCast(std::move(headWord), awst::WType::biguintType(), _loc);
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
		auto cast = awst::makeReinterpretCast(std::move(headWord), awst::WType::accountType(), _loc);
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
			int elemSize = ::puyasol::builder::TypeCoercion::computeEncodedElementSize(elemType);
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
				auto itob = awst::makeItob(std::move(elemCount), _loc);
				auto header = awst::makeExtract3(std::move(itob), awst::makeIntegerConstant("6", _loc), awst::makeIntegerConstant("2", _loc), _loc);
				// concat(header, elemBytes)
				auto arc4Bytes = awst::makeConcat(std::move(header), std::move(elemBytes), _loc);

				// ReinterpretCast to ARC4DynamicArray<elem>
				return awst::makeReinterpretCast(std::move(arc4Bytes), wtype, _loc);
			}
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
		// of FAILURE for corrupt-input decode cases.
		auto kind = wtype->kind();
		if (kind == awst::WTypeKind::ARC4DynamicArray
			|| kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4Struct
			|| kind == awst::WTypeKind::ARC4Tuple)
		{
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
