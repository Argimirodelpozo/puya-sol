/// @file AbiEncodeHeadTail.cpp
/// ABI encode top-level + head/tail framework, extracted from AbiCodecImpl.cpp:
///   - encodeArgsHeadTail: top-level head/tail layout for the
///     static-outer case (encodeWithSignature / encodeCall)
///   - rightPadTo32: pad shorter bytes to a 32-byte ABI word
///   - encodeDynamicTail: type-walk that emits the tail (or full value)
///     bytes; dispatches into per-shape encoders in AbiEncodeArrays.cpp
///   - encodeFromArc4Bytes: recursive bridge used when an outer encoder
///     has already extracted a child's bytes blob from an ARC4 container
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
	ContractContext& _ctx,
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
	std::shared_ptr<awst::Expression> currentOffset = awst::makeIntegerConstant(headSize, _loc);

	for (size_t i = _startIdx; i < args.size(); ++i)
	{
		auto const* solType = args[i]->annotation().type;
		auto expr = _ctx.buildExpr(*args[i]);
		if (!isDynArg(solType))
		{
			headParts.push_back(toPackedBytes(_ctx, std::move(expr), solType, false, _loc));
			continue;
		}
		auto offsetItob = awst::makeItob(currentOffset, _loc);
		headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

		auto tail = encodeDynamicTail(_ctx, std::move(expr), solType, _loc);
		auto tailLen = awst::makeLen(tail, _loc);

		currentOffset = awst::makeUInt64BinOp(
			std::move(currentOffset), awst::UInt64BinaryOperator::Add,
			std::move(tailLen), _loc);
		tailParts.push_back(std::move(tail));
	}

	auto encoded = concatByteExprs(std::move(headParts), _loc);
	if (!tailParts.empty())
	{
		auto tail = concatByteExprs(std::move(tailParts), _loc);
		encoded = awst::makeConcat(std::move(encoded), std::move(tail), _loc);
	}
	return encoded;
}



std::shared_ptr<awst::Expression> AbiEncoderBuilder::rightPadTo32(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	// Compute padding needed: (32 - (len % 32)) % 32
	// Then concat with bzero(padding)
	// For simplicity: concat(expr, bzero(32)), then extract first (len + padding) bytes
	// Actually simpler: concat(expr, bzero(31)), then extract first ((len + 31) / 32 * 32) bytes

	// len = len(expr)
	auto lenCall = awst::makeLen(_expr, _loc);

	// padded_len = ((len + 31) / 32) * 32
	auto len31 = awst::makeUInt64BinOp(std::move(lenCall),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant("31", _loc), _loc);

	auto div32 = awst::makeUInt64BinOp(std::move(len31),
		awst::UInt64BinaryOperator::FloorDiv,
		awst::makeIntegerConstant("32", _loc), _loc);

	auto paddedLen = awst::makeUInt64BinOp(std::move(div32),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);

	// concat(expr, bzero(31)) — ensure enough zeros for any padding
	auto padded = awst::makeRightPad(std::move(_expr), 31, _loc);

	// extract3(padded, 0, paddedLen)
	auto result = awst::makeExtract3(std::move(padded), awst::makeIntegerConstant("0", _loc), std::move(paddedLen), _loc);
	return result;
}

// ── encodeDynamicTail: [length as 32 bytes][data right-padded to 32] ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynamicTail(
	ContractContext& _ctx,
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
			auto lenCall = awst::makeLen(bytesExpr, _loc);
			auto lenItob = awst::makeItob(std::move(lenCall), _loc);
			auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

			// data right-padded to 32-byte boundary
			auto dataPadded = rightPadTo32(std::move(bytesExpr), _loc);

			// concat length + data
			return awst::makeConcat(std::move(lenPadded), std::move(dataPadded), _loc);
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

				auto rawLen = awst::makeLen(asBytes, _loc);

				auto two = awst::makeIntegerConstant("2", _loc);

				auto contentBytes = awst::makeUInt64BinOp(std::move(rawLen), awst::UInt64BinaryOperator::Sub, std::move(two), _loc);

				auto elemSize = awst::makeIntegerConstant(elemByteSize, _loc);

				auto lenExpr = awst::makeUInt64BinOp(std::move(contentBytes), awst::UInt64BinaryOperator::FloorDiv, std::move(elemSize), _loc);

				auto lenItob = awst::makeItob(std::move(lenExpr), _loc);
				auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

				auto bytesCast = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);
				auto stripHeader = awst::makeExtract(std::move(bytesCast), 2, 0, _loc);

				return awst::makeConcat(std::move(lenPadded), std::move(stripHeader), _loc);
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
			std::shared_ptr<awst::Expression> currentOffset = awst::makeIntegerConstant(headSize, _loc);

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

				auto fieldExpr = awst::makeFieldExpression(_expr, memberDecl->name(), arc4FieldType ? arc4FieldType : fieldNativeType, _loc);

				std::shared_ptr<awst::Expression> fieldValue = fieldExpr;
				if (arc4FieldType && arc4FieldType != fieldNativeType)
				{
					auto decode = awst::makeARC4Decode(std::move(fieldValue), fieldNativeType, _loc);
					fieldValue = std::move(decode);
				}

				if (!isDyn)
					headParts.push_back(toPackedBytes(_ctx, std::move(fieldValue), fieldSolType, false, _loc));
				else
				{
					auto offItob = awst::makeItob(currentOffset, _loc);
					headParts.push_back(leftPadBytes(std::move(offItob), 32, _loc));

					auto tail = encodeDynamicTail(_ctx, std::move(fieldValue), fieldSolType, _loc);
					auto tailLen = awst::makeLen(tail, _loc);

					currentOffset = awst::makeUInt64BinOp(
						std::move(currentOffset), awst::UInt64BinaryOperator::Add,
						std::move(tailLen), _loc);
					tailParts.push_back(std::move(tail));
				}
			}

			auto head = concatByteExprs(std::move(headParts), _loc);
			if (tailParts.empty())
				return head;
			auto tail = concatByteExprs(std::move(tailParts), _loc);
			return awst::makeConcat(std::move(head), std::move(tail), _loc);
		}
	}

	// Fallback: just pad to 32
	return toPackedBytes(_ctx, std::move(_expr), _solType, false, _loc);
}

// ── handleEncode: EVM ABI encode with head/tail encoding ──


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
	ContractContext& _ctx,
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
