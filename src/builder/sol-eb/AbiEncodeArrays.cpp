/// @file AbiEncodeArrays.cpp
/// ABI encode loops for arrays, extracted from AbiCodecImpl.cpp:
///   - encodeDynArrayPadSmallElems: T[] where T encodes < 32 bytes
///     (pad each element to a 32-byte word)
///   - encodeDynArrayDynElems: T[] where T is itself dynamic (build the
///     per-element head/tail layout inside the array body)
///   - encodeStaticArrayDynElems: T[N] where T is dynamic (same shape
///     without the length word)
#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-eb/AbiCodecHelpers.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{
using namespace abi_codec;
}
namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayPadSmallElems(
	ContractContext& _ctx,
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
	auto loopCond = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);
	auto body = awst::makeBlock(_loc);

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
		auto pad = awst::makeBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
		padded = bytesConcat(std::move(elem), std::move(pad), _loc);
	}
	else
	{
		// uint/bool/address: left-pad with zeros to 32 (high bytes).
		// bzero(32 - sz) ++ elem
		auto pad = awst::makeBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
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

	_ctx.prePendingStatements.push_back(
		awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	return accVar;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayDynElems(
	ContractContext& _ctx,
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
		awst::makeBzero(u64Const("0", _loc), _loc), _loc));

	// acc_tail = bzero(0)
	std::string tailName = "__abi_dynelem_tail_" + suffix;
	auto tailVar = awst::makeVarExpression(tailName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(tailVar,
		awst::makeBzero(u64Const("0", _loc), _loc), _loc));

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
	auto loopCond = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);
	auto body = awst::makeBlock(_loc);

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

		auto thenBlock = awst::makeBlock(_loc);
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

		body->body.push_back(awst::makeIfElse(
			std::move(cond), std::move(thenBlock), nullptr, _loc));
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

	_ctx.prePendingStatements.push_back(
		awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	// Build result: leftpad32(itob(outer_n)) ++ acc_head ++ acc_tail
	auto outerLenWord = leftPadBytes(u64Itob(nVar, _loc), 32, _loc);
	auto headTail = bytesConcat(headVar, tailVar, _loc);
	return bytesConcat(std::move(outerLenWord), std::move(headTail), _loc);
}


std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeStaticArrayDynElems(
	ContractContext& _ctx,
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
		awst::makeBzero(u64Const("0", _loc), _loc), _loc));
	std::string tailName = "__abi_sadyn_tail_" + suffix;
	auto tailVar = awst::makeVarExpression(tailName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(tailVar,
		awst::makeBzero(u64Const("0", _loc), _loc), _loc));

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
	auto loopCond = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt,
		u64Const(std::to_string(_n), _loc), _loc);
	auto body = awst::makeBlock(_loc);

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
		auto thenBlock = awst::makeBlock(_loc);
		auto i1 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto i1X2 = awst::makeUInt64BinOp(std::move(i1),
			awst::UInt64BinaryOperator::Mult, u64Const("2", _loc), _loc);
		auto nxtArcOff = bytesExtractU16(arrVar, std::move(i1X2), _loc);
		thenBlock->body.push_back(assignFresh(innEndVar, std::move(nxtArcOff), _loc));
		body->body.push_back(awst::makeIfElse(
			std::move(cond), std::move(thenBlock), nullptr, _loc));
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

	_ctx.prePendingStatements.push_back(
		awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

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

} // namespace puyasol::builder::eb
