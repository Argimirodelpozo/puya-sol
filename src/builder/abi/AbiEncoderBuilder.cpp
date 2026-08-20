/// @file AbiEncoderBuilder.cpp
/// Handles abi.encode*, abi.decode — extracted from FunctionCallBuilder.

#include "builder/abi/AbiEncoderBuilder.h"
#include "awst/NameGen.h"
#include "builder/storage/StorageMapper.h"
#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>

#include <functional>

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
	// Evaluate the source once: it feeds both the sign test (byte 0), its own
	// length, and the replace3 payload. makeEvalOnce wraps non-trivial exprs in
	// a SingleEvaluation so they evaluate exactly once.
	auto once = awst::makeEvalOnce(std::move(_bytes), _loc);
	// sign = top bit of byte 0 set?  (the value is canonical two's-complement
	// at its current width, so byte 0 carries the sign regardless of width)
	auto signByte = awst::makeBtoi(awst::makeExtract(once, 0, 1, _loc), _loc);
	auto isNeg = awst::makeNumericCompare(
		std::move(signByte), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(128, _loc), _loc);
	// base = 32 bytes of the sign fill (all-0xff for negative, all-0x00 else)
	auto ones = awst::makeBytesConstant(std::vector<uint8_t>(32, 0xffu), _loc);
	auto zeros = awst::makeBzero(32, _loc);
	auto base = awst::makeConditional(
		std::move(isNeg), std::move(ones), std::move(zeros),
		awst::WType::bytesType(), _loc);
	// overwrite the low `len(once)` bytes of the fill with the value:
	// replace3(base, 32 - len(once), once). For a 32-byte input start=0 (whole
	// value preserved) → idempotent; for an 8-byte input the high 24 keep the
	// sign fill.
	auto start = awst::makeUInt64BinOp(
		awst::makeIntegerConstant(32, _loc), awst::UInt64BinaryOperator::Sub,
		awst::makeLen(once, _loc), _loc);
	return awst::makeReplace3(std::move(base), std::move(start), once, _loc);
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
		// Packed address/contract stays the FULL 32-byte AVM account
		// (EVM packs 20). EVM_DIVERGENCE pinned by conversions/
		// encodepacked_widths: a 20-byte slice would TRUNCATE real accounts —
		// only literal-derived addresses would round-trip.
		auto cast = awst::makeAsBytes(std::move(_expr), _loc);
		bytesExpr = std::move(cast);
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
					auto bytesVar = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::bytesType(), _loc);
					};
					auto u64Var = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::uint64Type(), _loc);
					};
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
				int elemSize = builder::StorageMapper::computeEncodedElementSize(elemW);
				if (elemSize > 0 && elemSize != 32)
				{
					// __pkd_src = body; loop i<n: out ||= pad32(elem i)
					auto uniq = std::to_string(_callNode.id()) + "_" + std::to_string(argIdx);
					auto bytesVar = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::bytesType(), _loc);
					};
					auto u64Var = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::uint64Type(), _loc);
					};
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

namespace
{
/// Runtime dual-format abi.decode support for FLAT component lists (value
/// types + string/bytes). Externally-produced blobs — proxy/initialize(bytes)
/// parameters, off-chain encoders — arrive in EVM ABI layout (32-byte words,
/// u256 offset heads), which the ARC4-everywhere reinterpret cannot parse
/// ("extraction beyond length"; e741's initialize). The blob's layout is only
/// knowable at runtime, so emit BOTH decoders behind a sniff: every dynamic
/// member's head word must have 24 leading zero bytes and a word-aligned,
/// in-bounds offset — bytes an ARC4 encoding of the same components cannot
/// produce at those positions. ARC4 remains the native path (round-trips are
/// unchanged); nested aggregates keep ARC4-only.
struct EvmDecodeComp
{
	solidity::frontend::Type const* solType = nullptr;
	awst::WType const* wtype = nullptr;
	bool dynamic = false; // string/bytes member
};

/// Classify components; false if any member is outside the flat subset.
bool collectEvmDecodeComps(
	ContractContext& _ctx,
	std::vector<solidity::frontend::Type const*> const& _comps,
	std::vector<EvmDecodeComp>& _out)
{
	using namespace solidity::frontend;
	for (auto const* t: _comps)
	{
		if (!t)
			return false;
		EvmDecodeComp c;
		c.solType = t;
		c.wtype = _ctx.typeMapper.map(t);
		if (!c.wtype)
			return false;
		if (auto const* at = dynamic_cast<ArrayType const*>(t))
		{
			if (!at->isByteArrayOrString())
				return false; // nested array — ARC4-only
			c.dynamic = true;
		}
		else if (dynamic_cast<IntegerType const*>(t) || dynamic_cast<BoolType const*>(t)
			|| dynamic_cast<AddressType const*>(t) || dynamic_cast<EnumType const*>(t)
			|| dynamic_cast<ContractType const*>(t))
			c.dynamic = false;
		else
			return false; // structs, fixed bytes, fn ptrs … — ARC4-only
		_out.push_back(c);
	}
	return !_out.empty();
}

/// Build the native value of component `i` from the pinned EVM blob var.
std::shared_ptr<awst::Expression> evmDecodeComponent(
	std::function<std::shared_ptr<awst::Expression>()> const& _blob,
	EvmDecodeComp const& _c,
	size_t _i,
	awst::SourceLocation const& _loc)
{
	auto word = [&]() {
		return awst::makeExtract3(_blob(),
			awst::makeIntegerConstant(uint64_t{32 * _i}, _loc),
			awst::makeIntegerConstant(uint64_t{32}, _loc), _loc);
	};
	auto wordU64 = [&]() {
		return awst::makeBtoi(awst::makeExtractLastN(word(), 8, _loc), _loc);
	};
	if (_c.dynamic)
	{
		// head → [len32][data]; trim to len.
		auto off = wordU64();
		auto lenWord = awst::makeExtract3(_blob(), off,
			awst::makeIntegerConstant(uint64_t{32}, _loc), _loc);
		auto len = awst::makeBtoi(awst::makeExtractLastN(std::move(lenWord), 8, _loc), _loc);
		auto dataOff = awst::makeUInt64BinOp(wordU64(),
			awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(uint64_t{32}, _loc), _loc);
		auto data = awst::makeExtract3(_blob(), std::move(dataOff), std::move(len), _loc);
		if (_c.wtype == awst::WType::stringType())
			return awst::makeReinterpretCast(std::move(data),
				awst::WType::stringType(), _loc);
		return data;
	}
	if (_c.wtype == awst::WType::boolType())
		return awst::makeNumericCompare(wordU64(), awst::NumericComparison::Ne,
			awst::makeIntegerConstant(uint64_t{0}, _loc), _loc);
	if (_c.wtype == awst::WType::uint64Type())
		return wordU64();
	if (_c.wtype == awst::WType::accountType())
		// EVM word = 12 zero bytes ++ 20-byte address — exactly this project's
		// canonical 32-byte account form.
		return awst::makeReinterpretCast(word(), awst::WType::accountType(), _loc);
	if (_c.wtype == awst::WType::applicationType())
		return awst::makeReinterpretCast(wordU64(),
			awst::WType::applicationType(), _loc);
	// biguint (uint>64 / signed canonical-256 TC): the word IS the value.
	return awst::makeAsBiguint(word(), _loc);
}

/// The runtime layout sniff: blob long enough for the static head area, and
/// every dynamic head word EVM-shaped (24 zero prefix bytes, aligned offset,
/// in-bounds with its length word and data).
std::shared_ptr<awst::Expression> evmLayoutSniff(
	std::function<std::shared_ptr<awst::Expression>()> const& _blob,
	std::vector<EvmDecodeComp> const& _comps,
	awst::SourceLocation const& _loc)
{
	auto u64c = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc); };
	auto blobLen = [&]() { return awst::makeLen(_blob(), _loc); };
	std::shared_ptr<awst::Expression> cond = awst::makeNumericCompare(
		blobLen(), awst::NumericComparison::Gte,
		u64c(32 * _comps.size()), _loc);
	auto andWith = [&](std::shared_ptr<awst::Expression> more) {
		cond = awst::makeBoolBinOp(std::move(cond),
			awst::BinaryBooleanOperator::And, std::move(more), _loc);
	};
	bool anyDynamic = false;
	for (size_t i = 0; i < _comps.size(); ++i)
	{
		if (!_comps[i].dynamic)
			continue;
		anyDynamic = true;
		auto word = [&, i]() {
			return awst::makeExtract3(_blob(), u64c(32 * i), u64c(32), _loc);
		};
		auto off = [&, i]() {
			return awst::makeBtoi(awst::makeExtractLastN(word(), 8, _loc), _loc);
		};
		// 24-byte zero prefix (offsets beyond 2^64 are not real blobs).
		// Compared as biguint — 24 zero bytes ⇔ value 0.
		andWith(awst::makeNumericCompare(
			awst::makeAsBiguint(
				awst::makeExtract3(_blob(), u64c(32 * i), u64c(24), _loc), _loc),
			awst::NumericComparison::Eq,
			awst::makeIntegerConstant("0", _loc, awst::WType::biguintType()),
			_loc));
		// word-aligned …
		andWith(awst::makeNumericCompare(
			awst::makeUInt64BinOp(off(), awst::UInt64BinaryOperator::Mod,
				u64c(32), _loc),
			awst::NumericComparison::Eq, u64c(0), _loc));
		// … and the length word fits before the blob end.
		andWith(awst::makeNumericCompare(
			awst::makeUInt64BinOp(off(), awst::UInt64BinaryOperator::Add,
				u64c(32), _loc),
			awst::NumericComparison::Lte, blobLen(), _loc));
	}
	if (!anyDynamic)
	{
		// All-static: EVM layout is exactly 32 bytes per component (the ARC4
		// form only coincides when every member is 256-bit — then both parses
		// agree anyway).
		cond = awst::makeNumericCompare(blobLen(),
			awst::NumericComparison::Eq, u64c(32 * _comps.size()), _loc);
	}
	return cond;
}
} // namespace

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleDecode(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	auto* targetType = _ctx.typeMapper.map(_callNode.annotation().type);
	if (_callNode.arguments().empty())
		return nullptr;

	auto decoded = _ctx.buildExpr(*_callNode.arguments()[0]);

	// A dynamic bytes/string target is ARC4-encoded as byte[] (uint16 length + data). Its wtype is
	// `bytes`, which equals the `bytes` TARGET's wtype — but the value still needs ARC4Decode to strip
	// the length prefix, so abi.decode(abi.encode(b),(bytes)) round-trips to raw `b` not the encoded
	// form. (string already falls through here since its wtype `bytes` != target `string`; bytes did
	// not, so the short-circuit returned the length-prefixed encoding.)
	bool dynByteTarget = (targetType == awst::WType::bytesType()
		|| targetType == awst::WType::stringType());
	if (!targetType || (decoded->wtype == targetType && !dynByteTarget))
		return std::make_unique<GenericAbiResult>(_ctx, std::move(decoded));

	// ── ARC4-everywhere with a runtime EVM-layout escape hatch. The native
	// path reinterprets the bytes as the target's ARC4 encoding (round-trips
	// with abi.encode). For FLAT component lists a runtime sniff additionally
	// recognises EVM ABI layout — externally-encoded blobs like proxy
	// initialize(bytes) parameters — and takes an offset walk instead.
	{
		auto dataExpr = decoded;
		if (dataExpr->wtype != awst::WType::bytesType())
			dataExpr = awst::makeAsBytes(std::move(dataExpr), _loc);

		auto const* callType = _callNode.annotation().type;
		auto const* tupleType =
			dynamic_cast<solidity::frontend::TupleType const*>(callType);

		std::vector<solidity::frontend::Type const*> compTypes;
		if (tupleType)
			for (auto const* comp: tupleType->components())
				compTypes.push_back(comp);
		else
			compTypes.push_back(callType);

		// The native-ARC4 decode expression, over an arbitrary blob source.
		auto buildArc4Decode = [&](std::shared_ptr<awst::Expression> _blob)
			-> std::shared_ptr<awst::Expression> {
			if (tupleType)
			{
				std::vector<awst::WType const*> arc4Types;
				for (auto const* comp: tupleType->components())
					arc4Types.push_back(
						_ctx.typeMapper.mapToARC4Type(_ctx.typeMapper.map(comp)));
				auto const* arc4TupleT =
					_ctx.typeMapper.createType<awst::ARC4Tuple>(arc4Types);
				auto arc4Val = awst::makeReinterpretCast(
					std::move(_blob), arc4TupleT, _loc);
				return awst::makeARC4Decode(std::move(arc4Val), targetType, _loc);
			}
			auto const* arc4T = _ctx.typeMapper.mapToARC4Type(targetType);
			std::shared_ptr<awst::Expression> arc4Val =
				awst::makeReinterpretCast(std::move(_blob), arc4T, _loc);
			if (targetType == arc4T)
				return arc4Val;
			return awst::makeARC4Decode(std::move(arc4Val), targetType, _loc);
		};

		std::vector<EvmDecodeComp> comps;
		if (collectEvmDecodeComps(_ctx, compTypes, comps))
		{
			int uid = awst::NameGen::next("AbiEncoderBuilder.dualDecode");
			std::string bnm = "__abidec_b_" + std::to_string(uid);
			_ctx.preEffects().push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(bnm, awst::WType::bytesType(), _loc),
				std::move(dataExpr), _loc));
			auto blob = [&, bnm]() {
				return awst::makeVarExpression(bnm, awst::WType::bytesType(), _loc);
			};

			std::vector<std::string> compNames;
			auto compVar = [&](size_t i) {
				return awst::makeVarExpression(
					compNames[i], comps[i].wtype, _loc);
			};
			for (size_t i = 0; i < comps.size(); ++i)
				compNames.push_back(
					"__abidec_c" + std::to_string(i) + "_" + std::to_string(uid));

			auto evmBlk = awst::makeBlock(_loc);
			for (size_t i = 0; i < comps.size(); ++i)
				evmBlk->body.push_back(awst::makeAssignmentStatement(
					compVar(i), evmDecodeComponent(blob, comps[i], i, _loc), _loc));

			auto arcBlk = awst::makeBlock(_loc);
			if (tupleType)
			{
				auto targets = awst::makeTupleExpression(targetType, _loc);
				for (size_t i = 0; i < comps.size(); ++i)
					targets->items.push_back(compVar(i));
				arcBlk->body.push_back(awst::makeAssignmentStatement(
					std::move(targets), buildArc4Decode(blob()), _loc));
			}
			else
				arcBlk->body.push_back(awst::makeAssignmentStatement(
					compVar(0), buildArc4Decode(blob()), _loc));

			_ctx.preEffects().push_back(awst::makeIfElse(
				evmLayoutSniff(blob, comps, _loc),
				std::move(evmBlk), std::move(arcBlk), _loc));

			if (tupleType)
			{
				auto result = awst::makeTupleExpression(targetType, _loc);
				for (size_t i = 0; i < comps.size(); ++i)
					result->items.push_back(compVar(i));
				return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
			}
			return std::make_unique<GenericAbiResult>(_ctx, compVar(0));
		}

		// Unsupported shapes (nested aggregates, fixed bytes, …): ARC4-only.
		return std::make_unique<GenericAbiResult>(
			_ctx, buildArc4Decode(std::move(dataExpr)));
	}
}

// ── arc4EncodeValues / encodeArgsAsArc4 ──
// Shared ARC4 arg encoder for abi.encode + abi.encodeWith{Selector,Signature}.
// Internal repr is ARC4, so this is a thin wrapper over puya's codec (no EVM
// head/tail layout): 0 args → empty bytes, 1 → its ARC4 bytes, N → an ARC4 tuple.
//
// ── THE ENCODE-CONVENTION MAP (do not add a fifth copy) ──
// Four ARC4-encode entry points exist ON PURPOSE, one per width convention:
//   1. arc4EncodeValues (here, + the encodeArgsAsArc4 / arc4EncodeArgsAtParamTypes
//      wrappers): the abi.* BYTES family — encodes at the value's BACKING width
//      (uint16 arg → arc4.uint64/8B). Documented, test-guarded EVM_DIVERGENCE;
//      custom-error payloads ride arc4EncodeArgsAtParamTypes deliberately.
//   2. InnerCallHandlers::encodeArgToBytes: ApplicationArgs for real calls —
//      encodes at the callee's DECLARED param type when known (exact biguint
//      width, pad-to-width, dynamic-bytes header), nullptr → backing width.
//   3. ReturnRewriter's return-wire encoding: ABI return slots (signed →
//      sign-extended uint256, unsigned biguint → natural uintN, asm bodies
//      wrapped mod 2^N). Feeds the method's arc4 return type.
//   4. SolEmitStatement's event encoding: ARC-28 (biguint-backed ints collapse
//      to uint256 to match puya's event registration).
// A value encoded under one convention will NOT decode under another — that
// asymmetry is load-bearing (see custom-error-payload-width / encoding-model).

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeValues(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>> _vals,
	awst::SourceLocation const& _loc)
{
	if (_vals.empty())
		return awst::makeBytesConstant({}, _loc);

	// ARC4 type from the value's own native wtype (a canonical singleton — matches
	// what abi.decode lands on; native value is already canonical two's-complement,
	// so no extra sign-extension). Already-ARC4 values just reinterpret to bytes.
	auto toBytes = [&](std::shared_ptr<awst::Expression> _val)
		-> std::shared_ptr<awst::Expression>
	{
		auto const* arc4T = _ctx.typeMapper.mapToARC4Type(_val->wtype);
		if (_val->wtype == arc4T)
			return awst::makeAsBytes(std::move(_val), _loc);
		return awst::makeAsBytes(awst::makeARC4Encode(std::move(_val), arc4T, _loc), _loc);
	};

	if (_vals.size() == 1)
		return toBytes(std::move(_vals[0]));

	std::vector<awst::WType const*> nativeTypes, arc4Types;
	for (auto const& val : _vals)
	{
		nativeTypes.push_back(val->wtype);
		arc4Types.push_back(_ctx.typeMapper.mapToARC4Type(val->wtype));
	}
	auto const* wtupleT = _ctx.typeMapper.createType<awst::WTuple>(nativeTypes);
	auto tupleExpr = awst::makeTupleExpression(wtupleT, _loc);
	tupleExpr->items = std::move(_vals);
	auto const* arc4TupleT = _ctx.typeMapper.createType<awst::ARC4Tuple>(arc4Types);
	auto enc = awst::makeARC4Encode(std::move(tupleExpr), arc4TupleT, _loc);
	return awst::makeAsBytes(std::move(enc), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeArgsAsArc4(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	size_t _startIdx,
	awst::SourceLocation const& _loc)
{
	auto const& args = _callNode.arguments();
	std::vector<std::shared_ptr<awst::Expression>> vals;
	for (size_t i = _startIdx; i < args.size(); ++i)
		vals.push_back(_ctx.buildExpr(*args[i]));
	return arc4EncodeValues(_ctx, std::move(vals), _loc);
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

	// ARC4-everywhere: emit the ARC4 encoding of the arg tuple via the shared encoder.
	return std::make_unique<GenericAbiResult>(
		_ctx, encodeArgsAsArc4(_ctx, _callNode, 0, _loc));

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

// ── Loop-based EVM-ABI encoders for non-trivial dynamic-array shapes ──
//
// `encodeDynamicTail` handles the trivial cases inline. The two helpers
// below cover the cases that need runtime loops over array elements:
//   (b) per-element padding for small static elements (uint8[], etc.)
//   (c) head/tail re-encoding for nested dynamic elements (uint256[][], etc.)
// Both emit `while` loops into `_ctx.preEffects()` and return a
// fresh local var holding the EVM-ABI-encoded bytes.



} // namespace puyasol::builder::eb
