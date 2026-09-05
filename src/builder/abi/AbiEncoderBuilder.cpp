/// @file AbiEncoderBuilder.cpp
/// Handles abi.encode*, abi.decode — extracted from FunctionCallBuilder.

#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/AwstShorthand.h"
#include "Logger.h"
#include "builder/storage/StorageMapper.h"
#include "builder/abi/EvmAbiEncode.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arrayElemTo32(
	std::shared_ptr<awst::Expression> _elem,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using solidity::frontend::Type;
	auto cat = _solType ? _solType->category() : Type::Category::Integer;

	// bool → the EVM 0/1 word. The ARC4 backing carries the truth bit in
	// byte 0's HIGH bit (0x80), so a pad-in-place would encode 0x…80.
	if (cat == Type::Category::Bool)
	{
		std::shared_ptr<awst::Expression> cond;
		if (_elem->wtype == awst::WType::boolType())
			cond = std::move(_elem);
		else
			cond = awst::makeNumericCompare(
				awst::makeGetbit(
					awst::makeAsBytes(std::move(_elem), _loc),
					awst::makeZero(_loc), _loc),
				awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", _loc), _loc);
		auto sel = awst::makeIntrinsicCall(
			"select", awst::WType::uint64Type(), _loc);
		sel->stackArgs.push_back(awst::makeZero(_loc));
		sel->stackArgs.push_back(awst::makeOne(_loc));
		sel->stackArgs.push_back(std::move(cond));
		return leftPadBytes(awst::makeItob(std::move(sel), _loc), 32, _loc);
	}

	// Raw bytes at the element's backing width.
	std::shared_ptr<awst::Expression> bytesExpr;
	if (_elem->wtype == awst::WType::uint64Type())
		bytesExpr = awst::makeItob(std::move(_elem), _loc);
	else if (_elem->wtype == awst::WType::bytesType())
		bytesExpr = std::move(_elem);
	else
		bytesExpr = awst::makeAsBytes(std::move(_elem), _loc);

	// bytesN: EVM left-aligns fixed bytes — pad on the RIGHT.
	if (cat == Type::Category::FixedBytes)
	{
		auto const* fb =
			dynamic_cast<solidity::frontend::FixedBytesType const*>(_solType);
		int n = fb ? static_cast<int>(fb->numBytes()) : 32;
		if (n >= 32)
			return bytesExpr;
		return awst::makeConcat(
			std::move(bytesExpr), awst::makeBzero(32 - n, _loc), _loc);
	}
	if (cat == Type::Category::Address || cat == Type::Category::Contract)
		return leftPadBytes(
			awst::makeExtractLastN(std::move(bytesExpr), 20, _loc), 32, _loc);

	if (auto const* it =
			dynamic_cast<solidity::frontend::IntegerType const*>(_solType);
		it && it->isSigned())
		return signExtendBytesTo32(std::move(bytesExpr), _loc);
	return leftPadBytes(std::move(bytesExpr), 32, _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::leftPadBytes(
	std::shared_ptr<awst::Expression> _expr, int _n, awst::SourceLocation const& _loc)
{
	// Thin module-local alias for the canonical Node.h helper — retained
	// because the abi.encode* builders call it from 20+ sites.
	return awst::makeLeftPadToN(std::move(_expr), _n, _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::signExtendBytesTo32(
	std::shared_ptr<awst::Expression> _bytes, awst::SourceLocation const& _loc)
{
	return codec::signExtendToWord(std::move(_bytes), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::concatByteExprs(
	std::vector<std::shared_ptr<awst::Expression>> _parts, awst::SourceLocation const& _loc)
{
	if (_parts.empty())
		return awst::makeBytesConstant({}, _loc);
	auto result = std::move(_parts[0]);
	for (size_t i = 1; i < _parts.size(); ++i)
		result = awst::makeConcat(std::move(result), std::move(_parts[i]), _loc);
	return result;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeValuesAsEvmAbi(
	ContractContext& _ctx,
	std::vector<solidity::frontend::Type const*> const& _types,
	std::vector<std::shared_ptr<awst::Expression>> _values,
	awst::SourceLocation const& _loc)
{
	if (!abi::canEncodeEvmAbi(_types))
	{
		Logger::instance().error(
			"type is not representable in canonical Solidity ABI encoding", _loc);
		return awst::makeBytesConstant({}, _loc);
	}
	return abi::encodeEvmAbi(
		_ctx.typeMapper, _types, std::move(_values), _loc, _ctx.preEffects());
}

// ── toPackedBytes: convert expr to bytes with optional packed width ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::toPackedBytes(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _solType,
	bool _isPacked,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	int packedWidth = 0;
	if (_isPacked && _solType)
	{
		auto cat = _solType->category();
		if (cat == Type::Category::Integer)
		{
			auto const* intType = dynamic_cast<IntegerType const*>(_solType);
			if (intType) packedWidth = static_cast<int>(intType->numBits() / 8);
		}
		else if (cat == Type::Category::FixedBytes)
		{
			auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType);
			if (fbType) packedWidth = static_cast<int>(fbType->numBytes());
		}
		else if (cat == Type::Category::Bool)
			packedWidth = 1;
		else if (cat == Type::Category::Address || cat == Type::Category::Contract)
			packedWidth = 20;
		else if (cat == Type::Category::Enum)
		{
			// Enums pack as their underlying uint (uint8 for <=256 members),
			// not the 8-byte native word. Without this an enum in
			// abi.encodePacked occupied 8 bytes — corrupting the keccak hash
			// and shifting every following argument.
			auto const* enumType = dynamic_cast<EnumType const*>(_solType);
			if (enumType)
				if (auto const* enc = dynamic_cast<IntegerType const*>(enumType->encodingType()))
					packedWidth = static_cast<int>(enc->numBits() / 8);
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr;

	// Capture the input-was-already-bytes-typed flag BEFORE the moves
	// below mutate _expr. Used by the packed-width truncation pass below
	// to skip the `extract 8-N N` slice when the input already arrives
	// at the right N-byte length (e.g. `bytes1(0xff)` lowers in
	// FixedBytesType to a 1-byte value; re-extracting would `extract 7 1`
	// on a 1-byte buffer and overflow).
	bool inputAlreadyByteshaped = (_expr->wtype == awst::WType::bytesType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes));

	if (_expr->wtype == awst::WType::bytesType())
		bytesExpr = std::move(_expr);
	else if (_expr->wtype == awst::WType::stringType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto cast = awst::makeAsBytes(std::move(_expr), _loc);
		bytesExpr = std::move(cast);
	}
	else if (_expr->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_expr), _loc);
		// For non-packed (abi.encode), pad to the 32-byte ABI word. A SIGNED
		// <=64-bit value arrives as a 64-bit sign-extended uint64 (itob ->
		// 0xff..fffd for a negative); a plain leftpad would ZERO-fill the high
		// 24 bytes (0x00..00fffd != EVM's 0xff..fffd). Sign-extend instead — the
		// 0xff/0x00 pad is selected on the sign bit at runtime. (Unsigned and
		// packed keep the existing zero-pad / raw-width behaviour.)
		if (_isPacked)
			bytesExpr = std::move(itob);
		else if (auto const* it = dynamic_cast<IntegerType const*>(_solType);
			it && it->isSigned())
			bytesExpr = signExtendBytesTo32(std::move(itob), _loc);
		else
			bytesExpr = leftPadBytes(std::move(itob), 32, _loc);
	}
	else if (_expr->wtype == awst::WType::biguintType())
	{
		// Non-packed (abi.encode) pads the value to a 32-byte word. A SIGNED
		// sub-256 value (int128 etc.) must SIGN-extend, not zero-pad: a
		// negative aggregate field decodes to a non-canonical biguint (e.g.
		// int128(-7) → 16-byte 0xff…f9), and a plain leftpad would produce
		// 0x00…00fff9 instead of 0xff…fff9. signExtendSignedElement is a no-op
		// for unsigned / int256 / <=64-bit / already-canonical values, so it
		// is safe to apply unconditionally here. (Packed keeps the raw bytes;
		// the packed-width logic slices to the declared N.)
		if (!_isPacked)
			_expr = builder::TypeCoercion::signExtendSignedElement(
				std::move(_expr), _solType, _loc);
		auto cast = awst::makeAsBytes(std::move(_expr), _loc);
		bytesExpr = _isPacked ? std::move(cast) : leftPadBytes(std::move(cast), 32, _loc);
	}
	else if (_expr->wtype == awst::WType::accountType())
	{
		auto cast = awst::makeAsBytes(std::move(_expr), _loc);
		bytesExpr = std::move(cast);
		if (_isPacked && packedWidth == 20)
		{
			bytesExpr = awst::makeExtractLastN(std::move(bytesExpr), 20, _loc);
			inputAlreadyByteshaped = true;
		}
	}
	else if (_expr->wtype == awst::WType::boolType())
	{
		auto boolToInt = awst::makeIntrinsicCall("select", awst::WType::uint64Type(), _loc);
		boolToInt->stackArgs.push_back(awst::makeZero(_loc));
		boolToInt->stackArgs.push_back(awst::makeOne(_loc));
		boolToInt->stackArgs.push_back(std::move(_expr));

		auto itob = awst::makeItob(std::move(boolToInt), _loc);
		// For non-packed (abi.encode), pad to the 32-byte ABI word — mirrors
		// the uint64 branch. Without this a bool occupied only 8 bytes and
		// misaligned every following argument. Packed keeps the raw 8-byte
		// itob (the packed-width logic below slices it to 1 byte).
		bytesExpr = _isPacked ? std::move(itob) : leftPadBytes(std::move(itob), 32, _loc);
	}
	else
	{
		auto cast = awst::makeAsBytes(std::move(_expr), _loc);
		bytesExpr = std::move(cast);
	}

	// Packed width truncation/padding.
	//
	// The `extract(8 - N, N)` form below assumes `bytesExpr` is exactly 8
	// bytes (came from `itob` of a uint64), so we slice off the last N
	// to drop the high-zero padding. That assumption holds for the
	// uint64 / bool input branches (lines 93-118) but NOT for the
	// bytes-typed branches (85-92) where the input is already the right
	// length (e.g. `bytes1(0xff)` → 1 byte via FixedBytesType lowering's
	// own `extract 7 1`). Doing the extract again on a 1-byte value runs
	// `extract 7 1` over a 1-byte buffer → AVM `extraction start 7 is
	// beyond length: 1` revert. (Puya 5.8 was tolerant of this shape;
	// puya 5.9 surfaces it as a runtime error — see puyabug.md #4a.)
	//
	// `inputAlreadyByteshaped` was captured before the moves above.
	if (packedWidth > 0 && packedWidth != 8 && !inputAlreadyByteshaped)
	{
		if (packedWidth <= 8)
		{
			bytesExpr = awst::makeExtract(
				std::move(bytesExpr), 8 - packedWidth, packedWidth, _loc);
		}
		else
			bytesExpr = leftPadBytes(std::move(bytesExpr), packedWidth, _loc);
	}

	return bytesExpr;
}

// ── encodePacked / encode ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncodePacked(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	bool _isPacked,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _callNode.arguments();

	if (args.empty())
		return std::make_unique<GenericAbiResult>(
			_ctx, awst::makeBytesConstant({}, _loc));

	// Pack each argument, expanding arrays element-by-element for encodePacked
	auto packArg = [&](size_t argIdx) -> std::shared_ptr<awst::Expression> {
		auto const* solType = args[argIdx]->annotation().type;

		auto const* arrType = dynamic_cast<ArrayType const*>(solType);
		if (!arrType && solType && solType->category() == Type::Category::UserDefinedValueType)
		{
			auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType);
			if (udvt)
				arrType = dynamic_cast<ArrayType const*>(&udvt->underlyingType());
		}

		if (arrType && !arrType->isByteArrayOrString())
		{
			// T2: pinned — indexed once per element below; a call-valued
			// array expression must evaluate once.
			auto arrayExpr = awst::makeEvalOnce(_ctx.buildExpr(*args[argIdx]), _loc);
			auto const* elemSolType = arrType->baseType();

			if (!arrType->isDynamicallySized())
			{
				int len = static_cast<int>(arrType->length());
				std::shared_ptr<awst::Expression> packed;
				for (int j = 0; j < len; ++j)
				{
					auto idx = awst::makeIntegerConstant(j, _loc);

					// Use ARC4 element type if base is ARC4 array
					awst::WType const* indexWtype = _ctx.typeMapper.map(elemSolType);
					if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrayExpr->wtype))
						indexWtype = sa->elementType();
					else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(arrayExpr->wtype))
						indexWtype = da->elementType();
					auto indexExpr = awst::makeIndexExpression(arrayExpr, std::move(idx), indexWtype, _loc);

					// EVM packs each array element to a full 32-byte word
					// in BOTH `abi.encode` AND `abi.encodePacked` modes.
					// (The Solidity docs on non-standard packed are
					// misleading — they say "each element encoded as if a
					// single value, no padding", but the actual EVM
					// implementation pads array elements regardless.) For
					// scalar packed values, sub-32-byte types stay at
					// their natural width; only when used INSIDE an
					// array do they pad. Without this, e.g.
					// `abi.encodePacked(uint120[3])` produces 45 bytes
					// instead of EVM's 96, breaking the keccak hash
					// (see builtinFunctions/keccak256_packed_complex_types).
					// NOT toPackedBytes: its `extract(8-N, N)` truncation
					// assumes an 8-byte itob input, but elements arrive at
					// their ARC4 backing width (4 bytes for uint32) — the
					// extract paniced for every 1-7-byte element type. And
					// bytesN elements pad RIGHT, arc4-bool elements carry
					// the truth bit in the HIGH bit — arrayElemTo32 owns
					// all three rules.
					auto elemBytes = arrayElemTo32(
						std::move(indexExpr), elemSolType, _loc);
					if (!packed)
						packed = std::move(elemBytes);
					else
						packed = awst::makeConcat(std::move(packed), std::move(elemBytes), _loc);
				}
				// len==0: reuse the already-built expression — a second
				// buildExpr would run its side effects twice (T2).
				return packed ? packed : toPackedBytes(_ctx, arrayExpr, solType, _isPacked, _loc);
			}
			else
			{
				// abi.encodePacked of a DYNAMIC array: ARC4-encode then STRIP the 2-byte
				// length prefix. encodePacked concatenates the elements with NO length
				// header. EVM pads in-place array elements to the FULL 32-byte word
				// (same rule as the fixed-size branch above) — for 32-byte element
				// types the raw ARC4 body already IS that; sub-word elements (e.g.
				// uint120 → 15-byte ARC4 backing) are re-padded element-by-element in
				// a runtime loop. Without the strip, the keccak input gains a spurious
				// 2-byte prefix — this was silently corrupting EVERY Fiat-Shamir keccak in
				// the honk transcript (eta and all downstream challenges wrong).
				std::shared_ptr<awst::Expression> arc4 =
					awst::makeARC4Encode(std::move(arrayExpr), awst::WType::bytesType(), _loc);

				// bool[]: the ARC-4 body is BIT-packed (2-byte count +
				// ceil(n/8) bytes) — no byte stride exists, so the generic
				// strip below would emit the raw bit soup where EVM wants a
				// full 0/1 word per element. Expand bit-by-bit: element i is
				// bit 16+i of the encoded value (getbit counts MSB-first).
				// (Deliberately NOT fixed via computeEncodedElementSize —
				// teaching it arc4-bool=1 would corrupt its storage-codec
				// callers, which face the same bit-packing.)
				if (elemSolType && elemSolType->category() == Type::Category::Bool)
				{
					auto uniq = std::to_string(_callNode.id()) + "_" + std::to_string(argIdx);
					auto bytesVar = [&](std::string const& n) { return shorthand::bytesVar(n, _loc); };
					auto u64Var = [&](std::string const& n) { return shorthand::u64Var(n, _loc); };
					std::string src = "__pkd_src_" + uniq, out = "__pkd_out_" + uniq,
						iN = "__pkd_i_" + uniq, nN = "__pkd_n_" + uniq;
					auto& pre = _ctx.preEffects();
					pre.push_back(awst::makeAssignmentStatement(
						bytesVar(src), std::move(arc4), _loc));
					{
						auto count = awst::makeIntrinsicCall(
							"extract_uint16", awst::WType::uint64Type(), _loc);
						count->stackArgs.push_back(bytesVar(src));
						count->stackArgs.push_back(awst::makeZero(_loc));
						pre.push_back(awst::makeAssignmentStatement(
							u64Var(nN), std::move(count), _loc));
					}
					pre.push_back(awst::makeAssignmentStatement(
						bytesVar(out), awst::makeBytesConstant({}, _loc), _loc));
					pre.push_back(awst::makeAssignmentStatement(
						u64Var(iN), awst::makeZero(_loc), _loc));

					auto body = awst::makeBlock(_loc);
					{
						auto bit = awst::makeGetbit(bytesVar(src),
							awst::makeUInt64BinOp(
								awst::makeIntegerConstant("16", _loc),
								awst::UInt64BinaryOperator::Add, u64Var(iN), _loc),
							_loc);
						auto word = leftPadBytes(
							awst::makeItob(std::move(bit), _loc), 32, _loc);
						body->body.push_back(awst::makeAssignmentStatement(
							bytesVar(out),
							awst::makeConcat(bytesVar(out), std::move(word), _loc),
							_loc));
						body->body.push_back(awst::makeAssignmentStatement(
							u64Var(iN),
							awst::makeUInt64BinOp(u64Var(iN),
								awst::UInt64BinaryOperator::Add, awst::makeOne(_loc), _loc),
							_loc));
					}
					pre.push_back(awst::makeWhileLoop(
						awst::makeNumericCompare(u64Var(iN), awst::NumericComparison::Lt,
							u64Var(nN), _loc),
						std::move(body), _loc));
					return bytesVar(out);
				}

				awst::WType const* elemW = _ctx.typeMapper.mapSolTypeToARC4(elemSolType);
				int elemSize = builder::computeEncodedElementSize(elemW).fixedBytes<int>().value_or(0);
				bool const normalizeFullWidthElement = elemSolType
					&& (elemSolType->category() == Type::Category::Address
						|| elemSolType->category() == Type::Category::Contract);
				if (elemSize > 0 && (elemSize != 32 || normalizeFullWidthElement))
				{
					// __pkd_src = body; loop i<n: out ||= pad32(elem i)
					auto uniq = std::to_string(_callNode.id()) + "_" + std::to_string(argIdx);
					auto bytesVar = [&](std::string const& n) { return shorthand::bytesVar(n, _loc); };
					auto u64Var = [&](std::string const& n) { return shorthand::u64Var(n, _loc); };
					std::string src = "__pkd_src_" + uniq, out = "__pkd_out_" + uniq,
						iN = "__pkd_i_" + uniq, nN = "__pkd_n_" + uniq;
					auto& pre = _ctx.preEffects();
					pre.push_back(awst::makeAssignmentStatement(
						bytesVar(src), std::move(arc4), _loc));
					pre.push_back(awst::makeAssignmentStatement(
						u64Var(nN),
						awst::makeUInt64BinOp(
							awst::makeUInt64BinOp(awst::makeLen(bytesVar(src), _loc),
								awst::UInt64BinaryOperator::Sub,
								awst::makeIntegerConstant("2", _loc), _loc),
							awst::UInt64BinaryOperator::FloorDiv,
							awst::makeIntegerConstant(static_cast<uint64_t>(elemSize), _loc), _loc),
						_loc));
					pre.push_back(awst::makeAssignmentStatement(
						bytesVar(out), awst::makeBytesConstant({}, _loc), _loc));
					pre.push_back(awst::makeAssignmentStatement(
						u64Var(iN), awst::makeZero(_loc), _loc));

					auto body = awst::makeBlock(_loc);
					{
						auto pos = awst::makeUInt64BinOp(
							awst::makeIntegerConstant("2", _loc),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(u64Var(iN),
								awst::UInt64BinaryOperator::Mult,
								awst::makeIntegerConstant(static_cast<uint64_t>(elemSize), _loc), _loc),
							_loc);
						auto elem = awst::makeExtract3(bytesVar(src), std::move(pos),
							awst::makeIntegerConstant(static_cast<uint64_t>(elemSize), _loc), _loc);
						// bytesN pads RIGHT, signed sign-extends, else pads
						// LEFT — arrayElemTo32 owns the padding rules.
						auto elem32 = arrayElemTo32(std::move(elem), elemSolType, _loc);
						body->body.push_back(awst::makeAssignmentStatement(
							bytesVar(out),
							awst::makeConcat(bytesVar(out), std::move(elem32), _loc), _loc));
						body->body.push_back(awst::makeAssignmentStatement(
							u64Var(iN),
							awst::makeUInt64BinOp(u64Var(iN),
								awst::UInt64BinaryOperator::Add, awst::makeOne(_loc), _loc),
							_loc));
					}
					pre.push_back(awst::makeWhileLoop(
						awst::makeNumericCompare(u64Var(iN), awst::NumericComparison::Lt,
							u64Var(nN), _loc),
						std::move(body), _loc));
					return bytesVar(out);
				}

				auto lenExpr = awst::makeLen(arc4, _loc);
				auto lenMinus2 = awst::makeUInt64BinOp(
					std::move(lenExpr), awst::UInt64BinaryOperator::Sub,
					awst::makeIntegerConstant("2", _loc), _loc);
				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(arc4);
				extract->stackArgs.push_back(awst::makeIntegerConstant("2", _loc));
				extract->stackArgs.push_back(std::move(lenMinus2));
				return extract;
			}
		}

		return toPackedBytes(_ctx, _ctx.buildExpr(*args[argIdx]), solType, _isPacked, _loc);
	};

	auto result = packArg(0);
	for (size_t i = 1; i < args.size(); ++i)
		result = awst::makeConcat(std::move(result), packArg(i), _loc);
	return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
}

// ── decode ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleDecode(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	auto* targetType = _ctx.typeMapper.map(_callNode.annotation().type);
	if (_callNode.arguments().empty())
		return nullptr;

	auto decoded = _ctx.buildExpr(*_callNode.arguments()[0]);

	// Solidity's `abi.decode` is deterministic: its input is canonical EVM ABI,
	// independent of the contract's AVM entry profile. ARC4 decoding belongs at
	// explicit ARC4 transport/storage boundaries, never behind a byte-layout
	// heuristic. The recursive decoder is driven entirely by solc type facts.
	{
		auto data = decoded;
		if (data->wtype != awst::WType::bytesType())
			data = awst::makeAsBytes(std::move(data), _loc);
		auto const* callType = _callNode.annotation().type;
		auto const* tupleType =
			dynamic_cast<solidity::frontend::TupleType const*>(callType);
		std::vector<solidity::frontend::Type const*> components;
		if (tupleType)
			for (auto const* component: tupleType->components())
				components.push_back(component);
		else
			components.push_back(callType);
		if (!targetType || !abi::canDecodeEvmAbi(components))
		{
			Logger::instance().error(
				"type is not representable in canonical Solidity ABI decoding", _loc);
			return std::make_unique<GenericAbiResult>(
				_ctx, awst::makeBytesConstant({}, _loc));
		}
		auto value = abi::decodeEvmAbi(
			_ctx.typeMapper, std::move(data), components, targetType,
			_loc, _ctx.preEffects());
		return std::make_unique<GenericAbiResult>(_ctx, std::move(value));
	}

}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::decodeArc4(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	solidity::frontend::Expression const& _dataNode,
	awst::SourceLocation const& _loc)
{
	auto const* targetType = _ctx.typeMapper.map(_callNode.annotation().type);
	if (!targetType)
		return awst::makeBytesConstant({}, _loc);
	auto data = _ctx.buildExpr(_dataNode);
	if (data->wtype != awst::WType::bytesType())
		data = awst::makeAsBytes(std::move(data), _loc);

	auto const* tupleType = dynamic_cast<solidity::frontend::TupleType const*>(
		_callNode.annotation().type);
	awst::WType const* wireType = nullptr;
	if (tupleType)
	{
		std::vector<awst::WType const*> components;
		for (auto const* component: tupleType->components())
		{
			auto const* wireComponent =
				_ctx.typeMapper.mapSolTypeToARC4(component);
			if (!wireComponent)
			{
				Logger::instance().error(
					"type is not representable in ARC4 decoding", _loc);
				return awst::makeBytesConstant({}, _loc);
			}
			components.push_back(wireComponent);
		}
		wireType = _ctx.typeMapper.createType<awst::ARC4Tuple>(
			std::move(components));
	}
	else
		wireType = _ctx.typeMapper.mapSolTypeToARC4(
			_callNode.annotation().type);
	if (!wireType)
	{
		Logger::instance().error(
			"type is not representable in ARC4 decoding", _loc);
		return awst::makeBytesConstant({}, _loc);
	}
	auto wire = awst::makeARC4FromBytes(
		std::move(data), wireType, _loc, /*validate=*/true);
	if (awst::structurallyEquivalent(wireType, targetType))
		return wire;
	return awst::makeARC4Decode(std::move(wire), targetType, _loc);
}

namespace
{

std::shared_ptr<awst::Expression> arc4EncodeAtWireTypes(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>> _values,
	std::vector<awst::WType const*> _wireTypes,
	awst::SourceLocation const& _loc)
{
	if (_values.empty())
		return awst::makeBytesConstant({}, _loc);

	if (_values.size() == 1)
	{
		auto value = std::move(_values.front());
		if (awst::structurallyEquivalent(value->wtype, _wireTypes.front()))
			return awst::makeAsBytes(std::move(value), _loc);
		return awst::makeAsBytes(
			awst::makeARC4Encode(std::move(value), _wireTypes.front(), _loc), _loc);
	}

	// Puya's tuple codec sees only UIntEncoding(N), not the signed alias. A
	// negative intN is held as a wider two's-complement native integer and would
	// therefore look like an overflow when encoded recursively. Pre-encode signed
	// members so makeARC4Encode can trim them to their declared wire width.
	for (size_t i = 0; i < _values.size(); ++i)
		if (auto const* integer =
				dynamic_cast<awst::ARC4UIntN const*>(_wireTypes[i]);
			integer && integer->isSigned()
			&& !awst::structurallyEquivalent(_values[i]->wtype, _wireTypes[i]))
			_values[i] = awst::makeARC4Encode(
				std::move(_values[i]), _wireTypes[i], _loc);

	std::vector<awst::WType const*> nativeTypes;
	nativeTypes.reserve(_values.size());
	for (auto const& value: _values)
		nativeTypes.push_back(value->wtype);
	auto const* nativeTuple = _ctx.typeMapper.createType<awst::WTuple>(
		std::move(nativeTypes));
	auto tuple = awst::makeTupleExpression(nativeTuple, _loc);
	tuple->items = std::move(_values);
	auto const* wireTuple = _ctx.typeMapper.createType<awst::ARC4Tuple>(
		std::move(_wireTypes));
	return awst::makeAsBytes(
		awst::makeARC4Encode(std::move(tuple), wireTuple, _loc), _loc);
}

} // namespace

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeSolidityArgs(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& _args,
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Expression>> values;
	std::vector<awst::WType const*> wireTypes;
	values.reserve(_args.size());
	wireTypes.reserve(_args.size());
	for (auto const& argument: _args)
	{
		auto const* sourceType = argument->annotation().type;
		auto const* concreteType = sourceType;
		if (concreteType)
			if (auto const* mobile = concreteType->mobileType())
				concreteType = mobile;
		auto const* nativeType = _ctx.typeMapper.map(concreteType);
		auto const* wireType = _ctx.typeMapper.mapSolTypeToARC4(concreteType);
		if (!nativeType || !wireType)
		{
			Logger::instance().error(
				"type is not representable in ARC4 encoding", _loc);
			return awst::makeBytesConstant({}, _loc);
		}

		auto value = _ctx.buildExpr(*argument);
		if (value->wtype != nativeType)
			value = builder::ConversionPlan{
				sourceType,
				concreteType,
				nativeType,
				builder::ConversionPlan::Context::AbiArgument}.emit(
					std::move(value), _loc);
		values.push_back(std::move(value));
		wireTypes.push_back(wireType);
	}

	return arc4EncodeAtWireTypes(
		_ctx, std::move(values), std::move(wireTypes), _loc);
}

// Shared ARC4 value encoder for compiler-private transports. It is a thin
// wrapper over puya's codec (no EVM head/tail layout): 0 values → empty bytes,
// 1 → its ARC4 bytes, N → an ARC4 tuple.
//
// ── THE ENCODE-CONVENTION MAP ──
// Five ARC4-encode entry points exist ON PURPOSE, one per wire convention:
//   1. arc4EncodeSolidityArgs: public ARC4 stdlib facade — exact SOLIDITY width
//      (uint16 value → arc4.uint16/2B).
//   2. arc4EncodeValues: compiler-private payloads — BACKING width (uint16
//      value → arc4.uint64/8B); custom-error payloads ride
//      arc4EncodeArgsAtParamTypes deliberately.
//   3. InnerCallHandlers::encodeArgToBytes: ApplicationArgs for real calls —
//      encodes at the callee's DECLARED param type when known (exact biguint
//      width, pad-to-width, dynamic-bytes header), nullptr → backing width.
//   4. Return-wire encoding: ABI return slots (signed →
//      sign-extended uint256, unsigned biguint → natural uintN, asm bodies
//      wrapped mod 2^N). Feeds the method's arc4 return type.
//   5. SolEmitStatement's event encoding: ARC-28 (biguint-backed ints collapse
//      to uint256 to match puya's event registration).
// A value encoded under one convention will NOT decode under another — that
// asymmetry is load-bearing (see custom-error-payload-width / encoding-model).

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeValues(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>> _vals,
	awst::SourceLocation const& _loc)
{
	std::vector<awst::WType const*> arc4Types;
	arc4Types.reserve(_vals.size());
	for (auto const& val : _vals)
		arc4Types.push_back(_ctx.typeMapper.mapToARC4Type(val->wtype));
	return arc4EncodeAtWireTypes(
		_ctx, std::move(_vals), std::move(arc4Types), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeArgsAtParamTypes(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& _args,
	std::vector<solidity::frontend::Type const*> const& _paramTypes,
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Expression>> vals;
	for (size_t i = 0; i < _args.size(); ++i)
	{
		auto expr = _ctx.buildExpr(*_args[i]);
		// Coerce to the DECLARED param type so a literal lands on the param's ARC4
		// width (`7` → uint256/32B, not arc4.uint64/8B). No-op if already matching;
		// args past _paramTypes keep their value type.
		solidity::frontend::Type const* pt =
			i < _paramTypes.size() ? _paramTypes[i] : nullptr;
		if (pt)
			if (auto const* pw = _ctx.typeMapper.map(pt))
				expr = builder::ConversionPlan{
					_args[i]->annotation().type,
					pt,
					pw,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(expr), _loc);
		vals.push_back(std::move(expr));
	}
	return arc4EncodeValues(_ctx, std::move(vals), _loc);
}


std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncode(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _callNode.arguments();

	if (args.empty())
		return std::make_unique<GenericAbiResult>(
			_ctx, awst::makeBytesConstant({}, _loc));

	std::vector<solidity::frontend::Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (auto const& arg: args)
	{
		auto const* sourceType = arg->annotation().type;
		auto const* type = sourceType;
		// Solc leaves literals as rational/string-literal pseudo-types. Its
		// mobile type is the concrete ABI type Solidity assigns at this call.
		if (type)
			if (auto const* mobile = type->mobileType())
				type = mobile;
		types.push_back(type);
		auto value = _ctx.buildExpr(*arg);
		if (type)
			if (auto const* target = _ctx.typeMapper.map(type);
				target && value->wtype != target)
				value = builder::ConversionPlan{
					sourceType, type, target,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(value), _loc);
		values.push_back(std::move(value));
	}
	return std::make_unique<GenericAbiResult>(_ctx,
		encodeValuesAsEvmAbi(_ctx, types, std::move(values), _loc));

}

// ── Top-level dispatcher ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::tryHandle(
	ContractContext& _ctx,
	std::string const& _memberName,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	if (_memberName == "encodePacked")
		return handleEncodePacked(_ctx, _callNode, /*isPacked=*/true, _loc);
	if (_memberName == "encode")
		return handleEncode(_ctx, _callNode, _loc);
	if (_memberName == "encodeCall")
		return handleEncodeCall(_ctx, _callNode, _loc);
	if (_memberName == "encodeWithSelector")
		return handleEncodeWithSelector(_ctx, _callNode, _loc);
	if (_memberName == "encodeWithSignature")
		return handleEncodeWithSignature(_ctx, _callNode, _loc);
	if (_memberName == "decode")
		return handleDecode(_ctx, _callNode, _loc);
	return nullptr;
}

} // namespace puyasol::builder::eb
