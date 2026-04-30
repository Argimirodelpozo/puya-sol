/// @file AbiCodecImpl.cpp
/// ABI head/tail encoder + decoder internals — extracted from
/// AbiEncoderBuilder.cpp. The public dispatchers (handleEncode*, handleDecode,
/// tryHandle) and small helpers (leftPadBytes, concatByteExprs, toPackedBytes,
/// encodeArgAsARC4Bytes, buildARC4MethodSelector) remain in
/// AbiEncoderBuilder.cpp; this file holds the heavy codec internals:
///
///   - encodeArgsHeadTail (head/tail layout for static-outer encodings)
///   - decodeAbiValue, uint64FromAbiWord (decode helpers)
///   - rightPadTo32 (32-byte padding)
///   - encodeDynamicTail and the four element-encoder loops:
///       encodeDynArrayPadSmallElems, encodeDynArrayDynElems,
///       encodeStaticArrayDynElems, encodeFromArc4Bytes

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

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeArgsHeadTail(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	size_t _startIdx,
	awst::SourceLocation const& _loc)
{
	auto const& args = _callNode.arguments();
	// StringLiteralType is nominally static per Solidity's type system, but
	// its encoding is dynamic (length + data) — treat as dynamic so offsets
	// get emitted in the head.
	auto isDynArg = [](solidity::frontend::Type const* _t) {
		if (!_t) return false;
		return _t->isDynamicallyEncoded()
			|| _t->category() == solidity::frontend::Type::Category::StringLiteral;
	};
	bool hasDynamic = false;
	for (size_t i = _startIdx; i < args.size(); ++i)
		if (isDynArg(args[i]->annotation().type))
			hasDynamic = true;

	if (!hasDynamic)
	{
		std::vector<std::shared_ptr<awst::Expression>> parts;
		for (size_t i = _startIdx; i < args.size(); ++i)
			parts.push_back(toPackedBytes(_ctx, _ctx.buildExpr(*args[i]), args[i]->annotation().type, false, _loc));
		return concatByteExprs(std::move(parts), _loc);
	}

	// Head/tail encoding: each arg in head gets either its packed value
	// (static) or a 32-byte offset pointer (dynamic) into the tail.
	size_t numTrailing = args.size() - _startIdx;
	size_t headSize = numTrailing * 32;
	std::vector<std::shared_ptr<awst::Expression>> headParts;
	std::vector<std::shared_ptr<awst::Expression>> tailParts;
	std::shared_ptr<awst::Expression> currentOffset = awst::makeIntegerConstant(std::to_string(headSize), _loc);

	for (size_t i = _startIdx; i < args.size(); ++i)
	{
		auto const* solType = args[i]->annotation().type;
		auto expr = _ctx.buildExpr(*args[i]);
		if (!isDynArg(solType))
		{
			headParts.push_back(toPackedBytes(_ctx, std::move(expr), solType, false, _loc));
			continue;
		}
		auto offsetItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		offsetItob->stackArgs.push_back(currentOffset);
		headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

		auto tail = encodeDynamicTail(_ctx, std::move(expr), solType, _loc);
		auto tailLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		tailLen->stackArgs.push_back(tail);

		auto newOffset = std::make_shared<awst::UInt64BinaryOperation>();
		newOffset->sourceLocation = _loc;
		newOffset->wtype = awst::WType::uint64Type();
		newOffset->op = awst::UInt64BinaryOperator::Add;
		newOffset->left = std::move(currentOffset);
		newOffset->right = std::move(tailLen);
		currentOffset = std::move(newOffset);
		tailParts.push_back(std::move(tail));
	}

	auto encoded = concatByteExprs(std::move(headParts), _loc);
	if (!tailParts.empty())
	{
		auto tail = concatByteExprs(std::move(tailParts), _loc);
		auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		concat->stackArgs.push_back(std::move(encoded));
		concat->stackArgs.push_back(std::move(tail));
		encoded = std::move(concat);
	}
	return encoded;
}


std::shared_ptr<awst::Expression> AbiEncoderBuilder::uint64FromAbiWord(
	std::shared_ptr<awst::Expression> _word32,
	awst::SourceLocation const& _loc)
{
	// Last 8 bytes of a 32-byte word: extract(_word32, 24, 8) → btoi
	auto last8 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	last8->immediates = {24, 8};
	last8->stackArgs.push_back(std::move(_word32));

	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	btoi->stackArgs.push_back(std::move(last8));
	return btoi;
}

// ── decodeAbiValue: decode one value from EVM ABI bytes ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::decodeAbiValue(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _data,
	std::shared_ptr<awst::Expression> _offset,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// Extract the 32-byte head word at _offset
	auto headWord = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	headWord->stackArgs.push_back(_data);
	headWord->stackArgs.push_back(_offset);
	headWord->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));

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
		auto zero = awst::makeIntegerConstant("0", _loc);
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
		auto lenWord = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		lenWord->stackArgs.push_back(_data);
		lenWord->stackArgs.push_back(tailOffset);
		lenWord->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));

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
					awst::makeIntegerConstant(std::to_string(elemSize), _loc),
					_loc);

				// elemBytes = extract3(_data, dataStart, byteCount)
				auto elemBytes = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				elemBytes->stackArgs.push_back(_data);
				elemBytes->stackArgs.push_back(std::move(dataStart));
				elemBytes->stackArgs.push_back(std::move(byteCount));

				// arc4Header = extract3(itob(elemCount), 6, 2) — uint16 BE length
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(elemCount));
				auto header = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				header->stackArgs.push_back(std::move(itob));
				header->stackArgs.push_back(awst::makeIntegerConstant("6", _loc));
				header->stackArgs.push_back(awst::makeIntegerConstant("2", _loc));

				// concat(header, elemBytes)
				auto arc4Bytes = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				arc4Bytes->stackArgs.push_back(std::move(header));
				arc4Bytes->stackArgs.push_back(std::move(elemBytes));

				// ReinterpretCast to ARC4DynamicArray<elem>
				return awst::makeReinterpretCast(std::move(arc4Bytes), wtype, _loc);
			}
		}

		// Extract data bytes (length word is interpreted as byte count — correct
		// for bytes/string where elements are 1 byte each)
		auto dataBytes = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		dataBytes->stackArgs.push_back(_data);
		dataBytes->stackArgs.push_back(std::move(dataStart));
		dataBytes->stackArgs.push_back(std::move(elemCount));

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
			auto fromBytes = std::make_shared<awst::ARC4FromBytes>();
			fromBytes->sourceLocation = _loc;
			fromBytes->wtype = wtype;
			fromBytes->value = std::move(dataBytes);
			fromBytes->validate = false;
			return fromBytes;
		}
		return dataBytes;
	}

	// Fallback: ReinterpretCast the 32-byte word
	auto cast = awst::makeReinterpretCast(std::move(headWord), wtype, _loc);
	return cast;
}

// ── rightPadTo32: pad bytes to next 32-byte boundary ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::rightPadTo32(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	// Compute padding needed: (32 - (len % 32)) % 32
	// Then concat with bzero(padding)
	// For simplicity: concat(expr, bzero(32)), then extract first (len + padding) bytes
	// Actually simpler: concat(expr, bzero(31)), then extract first ((len + 31) / 32 * 32) bytes

	// len = len(expr)
	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenCall->stackArgs.push_back(_expr);

	// padded_len = ((len + 31) / 32) * 32
	auto len31 = std::make_shared<awst::UInt64BinaryOperation>();
	len31->sourceLocation = _loc;
	len31->wtype = awst::WType::uint64Type();
	len31->op = awst::UInt64BinaryOperator::Add;
	len31->left = std::move(lenCall);
	len31->right = awst::makeIntegerConstant("31", _loc);

	auto div32 = std::make_shared<awst::UInt64BinaryOperation>();
	div32->sourceLocation = _loc;
	div32->wtype = awst::WType::uint64Type();
	div32->op = awst::UInt64BinaryOperator::FloorDiv;
	div32->left = std::move(len31);
	div32->right = awst::makeIntegerConstant("32", _loc);

	auto paddedLen = std::make_shared<awst::UInt64BinaryOperation>();
	paddedLen->sourceLocation = _loc;
	paddedLen->wtype = awst::WType::uint64Type();
	paddedLen->op = awst::UInt64BinaryOperator::Mult;
	paddedLen->left = std::move(div32);
	paddedLen->right = awst::makeIntegerConstant("32", _loc);

	// concat(expr, bzero(31)) — ensure enough zeros for any padding
	auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	zeros->stackArgs.push_back(awst::makeIntegerConstant("31", _loc));

	auto padded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	padded->stackArgs.push_back(std::move(_expr));
	padded->stackArgs.push_back(std::move(zeros));

	// extract3(padded, 0, paddedLen)
	auto result = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	result->stackArgs.push_back(std::move(padded));
	result->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
	result->stackArgs.push_back(std::move(paddedLen));
	return result;
}

// ── encodeDynamicTail: [length as 32 bytes][data right-padded to 32] ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynamicTail(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// StringLiteralType: treat like bytes/string for encoding purposes.
	bool isStringLiteral = _solType
		&& _solType->category() == Type::Category::StringLiteral;

	// For bytes/string: [length][data padded to 32]
	if (isStringLiteral
		|| (dynamic_cast<ArrayType const*>(_solType) != nullptr
			&& dynamic_cast<ArrayType const*>(_solType)->isByteArrayOrString()))
	{
		{
			// Convert to bytes
			auto bytesExpr = std::move(_expr);
			if (bytesExpr->wtype != awst::WType::bytesType())
			{
				auto cast = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), _loc);
				bytesExpr = std::move(cast);
			}

			// length as 32-byte uint256
			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
			lenCall->stackArgs.push_back(bytesExpr);

			auto lenItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			lenItob->stackArgs.push_back(std::move(lenCall));
			auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

			// data right-padded to 32-byte boundary
			auto dataPadded = rightPadTo32(std::move(bytesExpr), _loc);

			// concat length + data
			auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			concat->stackArgs.push_back(std::move(lenPadded));
			concat->stackArgs.push_back(std::move(dataPadded));
			return concat;
		}
	}

	// Dynamic T[] array (non-byte elements). Three sub-cases:
	//   (a) Static element with byte size == 32: fast path — strip the
	//       ARC4 uint16 length header and prepend a uint256 length word.
	//       Body bytes are already EVM-ABI-aligned.
	//   (b) Static element with byte size < 32: per-element pad to 32
	//       (left-pad for uints/bool/address, right-pad for bytesN).
	//       Emits a runtime while loop.
	//   (c) Dynamic element (nested dynamic): full head/tail
	//       re-encoding with recursion via `encodeFromArc4Bytes`.
	//       Emits a runtime while loop.
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
		{
			auto const* elemSolType = arrType->baseType();
			bool elemIsDyn = elemSolType
				&& (elemSolType->isDynamicallyEncoded()
					|| elemSolType->category() == Type::Category::StringLiteral);

			unsigned elemByteSize = 32;
			bool isFixedBytes = false;
			if (auto const* intType = dynamic_cast<IntegerType const*>(elemSolType))
				elemByteSize = std::max(1u, intType->numBits() / 8);
			else if (auto const* fbType = dynamic_cast<FixedBytesType const*>(elemSolType))
			{
				elemByteSize = std::max(1u, (unsigned) fbType->numBytes());
				isFixedBytes = true;
			}
			else if (elemSolType
				&& elemSolType->category() == Type::Category::Bool)
				elemByteSize = 1;
			else if (elemSolType
				&& elemSolType->category() == Type::Category::Address)
				elemByteSize = 20;
			else if (auto const* innerArr = dynamic_cast<ArrayType const*>(elemSolType))
			{
				// Nested static array of static elements (e.g. uint256[3] as
				// elem of T[]). EVM packs each elem as N × 32 bytes — same as
				// our ARC4 packing — so the fast path can copy through if the
				// nested elem is itself fully static and 32-byte-aligned.
				// Only computable when the nested array isn't dynamic.
				if (!innerArr->isDynamicallyEncoded()
					&& !innerArr->isDynamicallySized()
					&& !innerArr->isByteArrayOrString())
				{
					unsigned innerLen = static_cast<unsigned>(innerArr->length());
					auto const* innerBase = innerArr->baseType();
					unsigned innerElemSize = 0;
					if (auto const* it2 = dynamic_cast<IntegerType const*>(innerBase))
						innerElemSize = std::max(1u, it2->numBits() / 8);
					else if (auto const* fb2 = dynamic_cast<FixedBytesType const*>(innerBase))
						innerElemSize = std::max(1u, (unsigned) fb2->numBytes());
					else if (innerBase && innerBase->category() == Type::Category::Address)
						innerElemSize = 20;
					else if (innerBase && innerBase->category() == Type::Category::Bool)
						innerElemSize = 1;
					if (innerElemSize == 32)
						elemByteSize = innerLen * 32;  // exact match: elem is N×32
				}
			}

			// (a) Fast path for elements whose ARC4-encoded byte width
			// already matches their EVM-ABI byte width (i.e. multiples of
			// 32 with no per-element padding needed). Includes uint256[],
			// bytes32[], and nested-static cases like uint256[3][] where
			// each elem is 96 bytes in both encodings.
			if (!elemIsDyn && elemByteSize > 0 && elemByteSize % 32 == 0)
			{
				auto arrayExpr = _expr;
				auto asBytes = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);

				auto rawLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
				rawLen->stackArgs.push_back(asBytes);

				auto two = awst::makeIntegerConstant("2", _loc);

				auto contentBytes = awst::makeUInt64BinOp(std::move(rawLen), awst::UInt64BinaryOperator::Sub, std::move(two), _loc);

				auto elemSize = awst::makeIntegerConstant(std::to_string(elemByteSize), _loc);

				auto lenExpr = awst::makeUInt64BinOp(std::move(contentBytes), awst::UInt64BinaryOperator::FloorDiv, std::move(elemSize), _loc);

				auto lenItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				lenItob->stackArgs.push_back(std::move(lenExpr));
				auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

				auto stripHeader = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				stripHeader->immediates = {2, 0};
				{
					auto bytesCast = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);
					stripHeader->stackArgs.push_back(std::move(bytesCast));
				}

				auto concatArr = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				concatArr->stackArgs.push_back(std::move(lenPadded));
				concatArr->stackArgs.push_back(std::move(stripHeader));
				return concatArr;
			}

			// (b) Small static element: per-element pad via runtime loop.
			if (!elemIsDyn && elemByteSize > 0 && elemByteSize < 32)
			{
				return encodeDynArrayPadSmallElems(
					_ctx, _expr, elemSolType, elemByteSize, isFixedBytes, _loc);
			}

			// (c) Dynamic element: head/tail re-encoding via runtime loop.
			if (elemIsDyn)
			{
				return encodeDynArrayDynElems(_ctx, _expr, elemSolType, _loc);
			}
		}
	}

	// Static array of dynamic elements (e.g. bytes[3], uint256[][3]).
	// EVM packs as `[uint256 offsets × N][bodies]` (no leading length).
	// ARC4 packs as `[uint16 offsets × N][bodies]`. Re-encode via
	// `encodeStaticArrayDynElems` (uses runtime loop similar to the
	// nested-dynamic case but with no length word and a compile-time n).
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (!arrType->isDynamicallySized()
			&& !arrType->isByteArrayOrString()
			&& arrType->isDynamicallyEncoded())
		{
			auto const* elemSolType = arrType->baseType();
			bool elemIsDyn = elemSolType
				&& (elemSolType->isDynamicallyEncoded()
					|| elemSolType->category() == Type::Category::StringLiteral);
			if (elemIsDyn)
			{
				unsigned n = static_cast<unsigned>(arrType->length());
				return encodeStaticArrayDynElems(_ctx, _expr, elemSolType, n, _loc);
			}
		}
	}

	// Struct: recursively encode fields with EVM head/tail layout.
	// For a struct S { T1 f1; T2 f2; ... }, the EVM ABI encoding is:
	//   head = concat(encode_field_i_static | offset_i_if_dynamic) for each i
	//   tail = concat(encode_dynamic_tail_i) for each dynamic i
	if (auto const* structType = dynamic_cast<StructType const*>(_solType))
	{
		auto const& structDef = structType->structDefinition();
		size_t numFields = structDef.members().size();
		if (numFields > 0)
		{
			size_t headSize = numFields * 32;

			std::vector<std::shared_ptr<awst::Expression>> headParts;
			std::vector<std::shared_ptr<awst::Expression>> tailParts;
			std::shared_ptr<awst::Expression> currentOffset = awst::makeIntegerConstant(std::to_string(headSize), _loc);

			for (auto const& memberDecl : structDef.members())
			{
				auto const* fieldSolType = memberDecl->type();
				bool isDyn = fieldSolType
					&& (fieldSolType->isDynamicallyEncoded()
						|| fieldSolType->category() == Type::Category::StringLiteral);

				// Build a FieldExpression that pulls the ARC4 field value out
				// of the struct's packed representation, then ARC4Decode to
				// the native wtype so downstream encoders see the "logical"
				// value (e.g. biguint for uint256 rather than arc4.uint256).
				auto* fieldNativeType = _ctx.typeMapper.map(fieldSolType);
				awst::WType const* arc4FieldType = nullptr;
				if (auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(_expr->wtype))
				{
					for (auto const& [fname, ftype]: arc4Struct->fields())
						if (fname == memberDecl->name())
						{
							arc4FieldType = ftype;
							break;
						}
				}

				auto fieldExpr = std::make_shared<awst::FieldExpression>();
				fieldExpr->sourceLocation = _loc;
				fieldExpr->base = _expr;
				fieldExpr->name = memberDecl->name();
				fieldExpr->wtype = arc4FieldType ? arc4FieldType : fieldNativeType;

				std::shared_ptr<awst::Expression> fieldValue = fieldExpr;
				if (arc4FieldType && arc4FieldType != fieldNativeType)
				{
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = _loc;
					decode->wtype = fieldNativeType;
					decode->value = std::move(fieldValue);
					fieldValue = std::move(decode);
				}

				if (!isDyn)
					headParts.push_back(toPackedBytes(_ctx, std::move(fieldValue), fieldSolType, false, _loc));
				else
				{
					auto offItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
					offItob->stackArgs.push_back(currentOffset);
					headParts.push_back(leftPadBytes(std::move(offItob), 32, _loc));

					auto tail = encodeDynamicTail(_ctx, std::move(fieldValue), fieldSolType, _loc);
					auto tailLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
					tailLen->stackArgs.push_back(tail);

					auto newOffset = std::make_shared<awst::UInt64BinaryOperation>();
					newOffset->sourceLocation = _loc;
					newOffset->wtype = awst::WType::uint64Type();
					newOffset->op = awst::UInt64BinaryOperator::Add;
					newOffset->left = std::move(currentOffset);
					newOffset->right = std::move(tailLen);
					currentOffset = std::move(newOffset);
					tailParts.push_back(std::move(tail));
				}
			}

			auto head = concatByteExprs(std::move(headParts), _loc);
			if (tailParts.empty())
				return head;
			auto tail = concatByteExprs(std::move(tailParts), _loc);
			auto result = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			result->stackArgs.push_back(std::move(head));
			result->stackArgs.push_back(std::move(tail));
			return result;
		}
	}

	// Fallback: just pad to 32
	return toPackedBytes(_ctx, std::move(_expr), _solType, false, _loc);
}

// ── handleEncode: EVM ABI encode with head/tail encoding ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayPadSmallElems(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _elemSolType,
	unsigned _elemByteSize,
	bool _isFixedBytes,
	awst::SourceLocation const& _loc)
{
	(void) _elemSolType;
	auto const bytesT = awst::WType::bytesType();
	auto const u64T = awst::WType::uint64Type();

	int tc = s_encLoopCounter++;
	auto suffix = std::to_string(tc);

	// arr_b = ReinterpretCast(_expr, bytes)
	std::string arrName = "__abi_smelem_in_" + suffix;
	auto arrVar = awst::makeVarExpression(arrName, bytesT, _loc);
	{
		auto cast = awst::makeReinterpretCast(_expr, bytesT, _loc);
		_ctx.prePendingStatements.push_back(assignFresh(arrVar, cast, _loc));
	}

	// n = (len(arr_b) - 2) / elemByteSize    (ARC4: 2-byte length header)
	std::string nName = "__abi_smelem_n_" + suffix;
	auto nVar = awst::makeVarExpression(nName, u64T, _loc);
	{
		auto rawLen = bytesLen(arrVar, _loc);
		auto minus2 = awst::makeUInt64BinOp(
			std::move(rawLen), awst::UInt64BinaryOperator::Sub, u64Const("2", _loc), _loc);
		auto n = awst::makeUInt64BinOp(
			std::move(minus2), awst::UInt64BinaryOperator::FloorDiv,
			u64Const(std::to_string(_elemByteSize), _loc), _loc);
		_ctx.prePendingStatements.push_back(assignFresh(nVar, n, _loc));
	}

	// acc = leftpad32(itob(n))           — leading uint256 length word
	std::string accName = "__abi_smelem_acc_" + suffix;
	auto accVar = awst::makeVarExpression(accName, bytesT, _loc);
	{
		auto padded = leftPadBytes(u64Itob(nVar, _loc), 32, _loc);
		_ctx.prePendingStatements.push_back(assignFresh(accVar, padded, _loc));
	}

	// i = 0
	std::string iName = "__abi_smelem_i_" + suffix;
	auto iVar = awst::makeVarExpression(iName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(iVar, u64Const("0", _loc), _loc));

	// while i < n: { elem = extract3(arr_b, 2 + i*sz, sz);
	//                acc = concat(acc, padded(elem)); i += 1; }
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// elem_off = 2 + i*sz
	auto iScaled = awst::makeUInt64BinOp(
		iVar, awst::UInt64BinaryOperator::Mult,
		u64Const(std::to_string(_elemByteSize), _loc), _loc);
	auto elemOff = awst::makeUInt64BinOp(
		u64Const("2", _loc), awst::UInt64BinaryOperator::Add,
		std::move(iScaled), _loc);

	// elem = extract3(arr_b, elem_off, sz)
	auto elem = bytesExtract3(arrVar, std::move(elemOff),
		u64Const(std::to_string(_elemByteSize), _loc), _loc);

	// padded = (left|right)pad32(elem)
	std::shared_ptr<awst::Expression> padded;
	if (_isFixedBytes)
	{
		// bytesN: right-pad with zeros to 32 (low bytes).
		// elem ++ bzero(32 - sz)
		auto pad = bytesBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
		padded = bytesConcat(std::move(elem), std::move(pad), _loc);
	}
	else
	{
		// uint/bool/address: left-pad with zeros to 32 (high bytes).
		// bzero(32 - sz) ++ elem
		auto pad = bytesBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
		padded = bytesConcat(std::move(pad), std::move(elem), _loc);
	}

	// acc = concat(acc, padded)
	body->body.push_back(assignFresh(accVar,
		bytesConcat(accVar, std::move(padded), _loc), _loc));

	// i += 1
	body->body.push_back(assignFresh(iVar,
		awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc),
		_loc));

	loop->loopBody = std::move(body);
	_ctx.prePendingStatements.push_back(std::move(loop));

	return accVar;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayDynElems(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _elemSolType,
	awst::SourceLocation const& _loc)
{
	auto const bytesT = awst::WType::bytesType();
	auto const u64T = awst::WType::uint64Type();

	int tc = s_encLoopCounter++;
	auto suffix = std::to_string(tc);

	// arr_b = ReinterpretCast(_expr, bytes)
	std::string arrName = "__abi_dynelem_in_" + suffix;
	auto arrVar = awst::makeVarExpression(arrName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(arrVar,
		awst::makeReinterpretCast(_expr, bytesT, _loc), _loc));

	// outer_n = extract_uint16(arr_b, 0)
	std::string nName = "__abi_dynelem_n_" + suffix;
	auto nVar = awst::makeVarExpression(nName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(nVar,
		bytesExtractU16(arrVar, u64Const("0", _loc), _loc), _loc));

	// total_bytes = len(arr_b)
	std::string totName = "__abi_dynelem_tot_" + suffix;
	auto totVar = awst::makeVarExpression(totName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(totVar,
		bytesLen(arrVar, _loc), _loc));

	// acc_head = bzero(0)
	std::string headName = "__abi_dynelem_head_" + suffix;
	auto headVar = awst::makeVarExpression(headName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(headVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));

	// acc_tail = bzero(0)
	std::string tailName = "__abi_dynelem_tail_" + suffix;
	auto tailVar = awst::makeVarExpression(tailName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(tailVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));

	// off = outer_n * 32             (initial running EVM-ABI offset)
	std::string offName = "__abi_dynelem_off_" + suffix;
	auto offVar = awst::makeVarExpression(offName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(offVar,
		awst::makeUInt64BinOp(nVar, awst::UInt64BinaryOperator::Mult,
			u64Const("32", _loc), _loc), _loc));

	// i = 0
	std::string iName = "__abi_dynelem_i_" + suffix;
	auto iVar = awst::makeVarExpression(iName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(iVar, u64Const("0", _loc), _loc));

	// While loop body
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// inner_arc4_off = extract_uint16(arr_b, 2 + i*2)
	std::string innArcOffName = "__abi_dynelem_iaoff_" + suffix;
	auto innArcOffVar = awst::makeVarExpression(innArcOffName, u64T, _loc);
	{
		auto iX2 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Mult,
			u64Const("2", _loc), _loc);
		auto pos = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(iX2), _loc);
		body->body.push_back(assignFresh(innArcOffVar,
			bytesExtractU16(arrVar, std::move(pos), _loc), _loc));
	}

	// inner_start = 2 + inner_arc4_off
	std::string innStartName = "__abi_dynelem_istart_" + suffix;
	auto innStartVar = awst::makeVarExpression(innStartName, u64T, _loc);
	body->body.push_back(assignFresh(innStartVar,
		awst::makeUInt64BinOp(u64Const("2", _loc), awst::UInt64BinaryOperator::Add,
			innArcOffVar, _loc), _loc));

	// inner_end = if (i+1 < n) then 2 + extract_uint16(arr_b, 2 + (i+1)*2)
	//             else total_bytes
	// Implementation: compute next_off = 2 + extract_uint16(arr_b, 2 + (i+1)*2)
	// when i+1 < n; else next_off = total_bytes. Use a temp + IfElse.
	std::string innEndName = "__abi_dynelem_iend_" + suffix;
	auto innEndVar = awst::makeVarExpression(innEndName, u64T, _loc);
	{
		// Default: inner_end = total_bytes
		body->body.push_back(assignFresh(innEndVar, totVar, _loc));

		// if (i+1 < n) inner_end = 2 + extract_uint16(arr_b, 2 + (i+1)*2)
		auto iPlus1 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto cond = awst::makeNumericCompare(iPlus1, awst::NumericComparison::Lt, nVar, _loc);

		auto thenBlock = std::make_shared<awst::Block>();
		thenBlock->sourceLocation = _loc;
		auto iPlus1Again = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto i1X2 = awst::makeUInt64BinOp(std::move(iPlus1Again),
			awst::UInt64BinaryOperator::Mult, u64Const("2", _loc), _loc);
		auto pos = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(i1X2), _loc);
		auto nxtArcOff = bytesExtractU16(arrVar, std::move(pos), _loc);
		auto nxtStart = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(nxtArcOff), _loc);
		thenBlock->body.push_back(assignFresh(innEndVar, std::move(nxtStart), _loc));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(cond);
		ifStmt->ifBranch = std::move(thenBlock);
		body->body.push_back(std::move(ifStmt));
	}

	// inner_size = inner_end - inner_start
	std::string innSizeName = "__abi_dynelem_isz_" + suffix;
	auto innSizeVar = awst::makeVarExpression(innSizeName, u64T, _loc);
	body->body.push_back(assignFresh(innSizeVar,
		awst::makeUInt64BinOp(innEndVar, awst::UInt64BinaryOperator::Sub,
			innStartVar, _loc), _loc));

	// inner_bytes = extract3(arr_b, inner_start, inner_size)
	std::string innBytesName = "__abi_dynelem_ib_" + suffix;
	auto innBytesVar = awst::makeVarExpression(innBytesName, bytesT, _loc);
	body->body.push_back(assignFresh(innBytesVar,
		bytesExtract3(arrVar, innStartVar, innSizeVar, _loc), _loc));

	// inner_evm = encodeFromArc4Bytes(inner_bytes, _elemSolType)
	// Note: this recursive call may itself emit prePending statements.
	// Since we're inside a loop body (Block), we need to capture those
	// and inline them into the body — they shouldn't escape to the outer
	// function-level prePending. Use a temporary swap of prePending to
	// collect inner-emitter-emitted statements, then prepend them to the
	// loop body before this assignment.
	std::string innEvmName = "__abi_dynelem_iev_" + suffix;
	auto innEvmVar = awst::makeVarExpression(innEvmName, bytesT, _loc);
	{
		std::vector<std::shared_ptr<awst::Statement>> savedPre;
		savedPre.swap(_ctx.prePendingStatements);
		auto innEvm = encodeFromArc4Bytes(_ctx, innBytesVar, _elemSolType, _loc);
		// Splice any child-emitted prePending statements into body BEFORE
		// the assignment that consumes them.
		for (auto& s: _ctx.prePendingStatements)
			body->body.push_back(std::move(s));
		_ctx.prePendingStatements = std::move(savedPre);
		body->body.push_back(assignFresh(innEvmVar, std::move(innEvm), _loc));
	}

	// acc_head = concat(acc_head, leftpad32(itob(off)))
	{
		auto offPadded = leftPadBytes(u64Itob(offVar, _loc), 32, _loc);
		body->body.push_back(assignFresh(headVar,
			bytesConcat(headVar, std::move(offPadded), _loc), _loc));
	}

	// acc_tail = concat(acc_tail, inner_evm)
	body->body.push_back(assignFresh(tailVar,
		bytesConcat(tailVar, innEvmVar, _loc), _loc));

	// off += len(inner_evm)
	body->body.push_back(assignFresh(offVar,
		awst::makeUInt64BinOp(offVar, awst::UInt64BinaryOperator::Add,
			bytesLen(innEvmVar, _loc), _loc), _loc));

	// i += 1
	body->body.push_back(assignFresh(iVar,
		awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc), _loc));

	loop->loopBody = std::move(body);
	_ctx.prePendingStatements.push_back(std::move(loop));

	// Build result: leftpad32(itob(outer_n)) ++ acc_head ++ acc_tail
	auto outerLenWord = leftPadBytes(u64Itob(nVar, _loc), 32, _loc);
	auto headTail = bytesConcat(headVar, tailVar, _loc);
	return bytesConcat(std::move(outerLenWord), std::move(headTail), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeStaticArrayDynElems(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _elemSolType,
	unsigned _n,
	awst::SourceLocation const& _loc)
{
	auto const bytesT = awst::WType::bytesType();
	auto const u64T = awst::WType::uint64Type();

	int tc = s_encLoopCounter++;
	auto suffix = std::to_string(tc);

	// arr_b = ReinterpretCast(_expr, bytes)
	std::string arrName = "__abi_sadyn_in_" + suffix;
	auto arrVar = awst::makeVarExpression(arrName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(arrVar,
		awst::makeReinterpretCast(_expr, bytesT, _loc), _loc));

	// total_bytes = len(arr_b)
	std::string totName = "__abi_sadyn_tot_" + suffix;
	auto totVar = awst::makeVarExpression(totName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(totVar,
		bytesLen(arrVar, _loc), _loc));

	// acc_head = bzero(0); acc_tail = bzero(0)
	std::string headName = "__abi_sadyn_head_" + suffix;
	auto headVar = awst::makeVarExpression(headName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(headVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));
	std::string tailName = "__abi_sadyn_tail_" + suffix;
	auto tailVar = awst::makeVarExpression(tailName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(tailVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));

	// off = n * 32  (initial running EVM-ABI offset; n head slots × 32B)
	std::string offName = "__abi_sadyn_off_" + suffix;
	auto offVar = awst::makeVarExpression(offName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(offVar,
		u64Const(std::to_string(_n * 32), _loc), _loc));

	// i = 0
	std::string iName = "__abi_sadyn_i_" + suffix;
	auto iVar = awst::makeVarExpression(iName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(iVar, u64Const("0", _loc), _loc));

	// While loop body — same structure as encodeDynArrayDynElems but the
	// ARC4 offset table starts at byte 0 (no length header) and `n` is
	// compile-time fixed.
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt,
		u64Const(std::to_string(_n), _loc), _loc);
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// inner_arc4_off = extract_uint16(arr_b, i*2)
	std::string innArcOffName = "__abi_sadyn_iaoff_" + suffix;
	auto innArcOffVar = awst::makeVarExpression(innArcOffName, u64T, _loc);
	{
		auto iX2 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Mult,
			u64Const("2", _loc), _loc);
		body->body.push_back(assignFresh(innArcOffVar,
			bytesExtractU16(arrVar, std::move(iX2), _loc), _loc));
	}

	// inner_start = inner_arc4_off  (offsets in static-of-dyn ARC4 are
	// relative to the start of the array, not body — there's no length
	// header to skip past).
	std::string innStartName = "__abi_sadyn_istart_" + suffix;
	auto innStartVar = awst::makeVarExpression(innStartName, u64T, _loc);
	body->body.push_back(assignFresh(innStartVar, innArcOffVar, _loc));

	// inner_end = if (i+1 < n) then extract_uint16(arr_b, (i+1)*2)
	//             else total_bytes
	std::string innEndName = "__abi_sadyn_iend_" + suffix;
	auto innEndVar = awst::makeVarExpression(innEndName, u64T, _loc);
	{
		body->body.push_back(assignFresh(innEndVar, totVar, _loc));
		auto iPlus1 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto cond = awst::makeNumericCompare(iPlus1,
			awst::NumericComparison::Lt, u64Const(std::to_string(_n), _loc), _loc);
		auto thenBlock = std::make_shared<awst::Block>();
		thenBlock->sourceLocation = _loc;
		auto i1 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto i1X2 = awst::makeUInt64BinOp(std::move(i1),
			awst::UInt64BinaryOperator::Mult, u64Const("2", _loc), _loc);
		auto nxtArcOff = bytesExtractU16(arrVar, std::move(i1X2), _loc);
		thenBlock->body.push_back(assignFresh(innEndVar, std::move(nxtArcOff), _loc));
		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(cond);
		ifStmt->ifBranch = std::move(thenBlock);
		body->body.push_back(std::move(ifStmt));
	}

	// inner_size = inner_end - inner_start
	std::string innSizeName = "__abi_sadyn_isz_" + suffix;
	auto innSizeVar = awst::makeVarExpression(innSizeName, u64T, _loc);
	body->body.push_back(assignFresh(innSizeVar,
		awst::makeUInt64BinOp(innEndVar, awst::UInt64BinaryOperator::Sub,
			innStartVar, _loc), _loc));

	// inner_bytes = extract3(arr_b, inner_start, inner_size)
	std::string innBytesName = "__abi_sadyn_ib_" + suffix;
	auto innBytesVar = awst::makeVarExpression(innBytesName, bytesT, _loc);
	body->body.push_back(assignFresh(innBytesVar,
		bytesExtract3(arrVar, innStartVar, innSizeVar, _loc), _loc));

	// inner_evm = encodeFromArc4Bytes(inner_bytes, _elemSolType)  (recursive)
	std::string innEvmName = "__abi_sadyn_iev_" + suffix;
	auto innEvmVar = awst::makeVarExpression(innEvmName, bytesT, _loc);
	{
		std::vector<std::shared_ptr<awst::Statement>> savedPre;
		savedPre.swap(_ctx.prePendingStatements);
		auto innEvm = encodeFromArc4Bytes(_ctx, innBytesVar, _elemSolType, _loc);
		for (auto& s: _ctx.prePendingStatements)
			body->body.push_back(std::move(s));
		_ctx.prePendingStatements = std::move(savedPre);
		body->body.push_back(assignFresh(innEvmVar, std::move(innEvm), _loc));
	}

	// acc_head = concat(acc_head, leftpad32(itob(off)))
	{
		auto offPadded = leftPadBytes(u64Itob(offVar, _loc), 32, _loc);
		body->body.push_back(assignFresh(headVar,
			bytesConcat(headVar, std::move(offPadded), _loc), _loc));
	}

	// acc_tail = concat(acc_tail, inner_evm)
	body->body.push_back(assignFresh(tailVar,
		bytesConcat(tailVar, innEvmVar, _loc), _loc));

	// off += len(inner_evm)
	body->body.push_back(assignFresh(offVar,
		awst::makeUInt64BinOp(offVar, awst::UInt64BinaryOperator::Add,
			bytesLen(innEvmVar, _loc), _loc), _loc));

	// i += 1
	body->body.push_back(assignFresh(iVar,
		awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc), _loc));

	loop->loopBody = std::move(body);
	_ctx.prePendingStatements.push_back(std::move(loop));

	// Result: acc_head ++ acc_tail (no length word)
	return bytesConcat(headVar, tailVar, _loc);
}

// Recursive entry point used from inside loop bodies. The caller has
// already extracted a bytes blob from a parent ARC4 container (so the
// expression's wtype is `bytes`); this method re-types it via
// ReinterpretCast to whatever ARC4 wtype the inner Solidity type maps
// to, so the existing `encodeDynamicTail` branches (struct → field
// access, dyn-array → length+body, etc.) see a properly-typed value
// they can structurally walk. Without this cast, e.g. the struct
// branch's `FieldExpression` constructor would fail its assertion that
// the base wtype is `ARC4Struct | WTuple`.
std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeFromArc4Bytes(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _bytesExpr,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	auto* nativeType = _ctx.typeMapper.map(_solType);
	auto const* arc4Type = _ctx.typeMapper.mapToARC4Type(nativeType);
	auto recast = awst::makeReinterpretCast(std::move(_bytesExpr), arc4Type, _loc);
	return encodeDynamicTail(_ctx, std::move(recast), _solType, _loc);
}

} // namespace puyasol::builder::eb
