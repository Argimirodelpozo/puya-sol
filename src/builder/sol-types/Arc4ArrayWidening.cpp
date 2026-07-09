#include "builder/sol-types/Arc4ArrayWidening.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"

#include <string>
#include <vector>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (_value->wtype != awst::WType::uint64Type()) return nullptr;
	auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(_targetType);
	if (!arc4) return nullptr;
	int const bits = arc4->n();
	if (bits >= 64 || bits % 8 != 0) return nullptr;
	int const nBytes = bits / 8;
	auto bytes = awst::makeItob(std::move(_value), _loc);
	auto low = awst::makeExtract3(std::move(bytes),
		awst::makeIntegerConstant(8 - nBytes, _loc),
		awst::makeIntegerConstant(nBytes, _loc), _loc);
	return awst::makeReinterpretCast(std::move(low), _targetType, _loc);
}

std::shared_ptr<awst::Expression> tryWidenArc4StaticArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	awst::SourceLocation const& _loc)
{
	auto const* srcArr = dynamic_cast<awst::ARC4StaticArray const*>(_sourceType);
	auto const* tgtArr = dynamic_cast<awst::ARC4StaticArray const*>(_targetType);
	if (!srcArr || !tgtArr) return nullptr;
	if (srcArr->arraySize() != tgtArr->arraySize()) return nullptr;
	// Element type descriptors {bits, isSigned} (nullopt for non-int elements).
	auto const src = SolIntType::fromArc4(srcArr->elementType());
	auto const tgt = SolIntType::fromArc4(tgtArr->elementType());
	if (!src || !tgt) return nullptr;
	int const srcBits = static_cast<int>(src->bits);
	int const tgtBits = static_cast<int>(tgt->bits);
	if (srcBits >= tgtBits || srcBits % 8 != 0 || tgtBits % 8 != 0) return nullptr;
	int const srcBytes = srcBits / 8;
	int const tgtBytes = tgtBits / 8;
	int const padBytes = tgtBytes - srcBytes;
	int const count = static_cast<int>(srcArr->arraySize());
	bool const isSigned = src->isSigned;

	std::shared_ptr<awst::Expression> result;
	for (int i = 0; i < count; ++i)
	{
		// Per-element source bytes: extract3(src, i*srcBytes, srcBytes).
		auto srcByte = awst::makeExtract3(
			_mkSourceBytes(),
			awst::makeIntegerConstant(i * srcBytes, _loc),
			awst::makeIntegerConstant(srcBytes, _loc),
			_loc);

		std::shared_ptr<awst::Expression> widened;
		if (isSigned)
		{
			// Sign-extend: prepend 0xFF*padBytes if the high byte's bit 7 is
			// set, else 0x00*padBytes. We look at the first byte of the slice
			// (always the high byte in big-endian ARC4 encoding).
			auto signByte = awst::makeExtract3(
				srcByte,
				awst::makeIntegerConstant(0, _loc),
				awst::makeIntegerConstant(1, _loc),
				_loc);
			auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
			auto isNeg = awst::makeNumericCompare(
				std::move(signByteVal),
				awst::NumericComparison::Gte,
				awst::makeIntegerConstant(128, _loc),
				_loc);
			std::vector<uint8_t> ffPad(padBytes, 0xFFu);
			std::vector<uint8_t> zeroPad(padBytes, 0x00u);
			auto prepend = awst::makeConditional(
				std::move(isNeg),
				awst::makeBytesConstant(std::move(ffPad), _loc),
				awst::makeBytesConstant(std::move(zeroPad), _loc),
				awst::WType::bytesType(),
				_loc);
			widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
		}
		else
		{
			// Unsigned: prepend zeros.
			auto prepend = awst::makeBzero(padBytes, _loc);
			widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
		}

		if (!result) result = std::move(widened);
		else result = awst::makeConcat(std::move(result), std::move(widened), _loc);
	}

	return awst::makeReinterpretCast(std::move(result), _targetType, _loc);
}

std::shared_ptr<awst::Expression> tryWidenArc4DynamicArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	std::function<void(std::shared_ptr<awst::Statement>)> _emit,
	awst::SourceLocation const& _loc)
{
	auto const* srcArr = dynamic_cast<awst::ARC4DynamicArray const*>(_sourceType);
	auto const* tgtArr = dynamic_cast<awst::ARC4DynamicArray const*>(_targetType);
	if (!srcArr || !tgtArr) return nullptr;
	auto const src = SolIntType::fromArc4(srcArr->elementType());
	auto const tgt = SolIntType::fromArc4(tgtArr->elementType());
	if (!src || !tgt) return nullptr;
	int const srcBits = static_cast<int>(src->bits);
	int const tgtBits = static_cast<int>(tgt->bits);
	if (srcBits >= tgtBits || srcBits % 8 != 0 || tgtBits % 8 != 0) return nullptr;
	int const srcBytes = srcBits / 8;
	int const padBytes = (tgtBits - srcBits) / 8;
	bool const isSigned = src->isSigned;

	int const n = awst::NameGen::next("Arc4ArrayWidening.s_dwCounter");
	auto u64 = awst::WType::uint64Type();
	auto bytesT = awst::WType::bytesType();
	std::string const lenN = "__dwiden_len_" + std::to_string(n);
	std::string const idxN = "__dwiden_i_" + std::to_string(n);
	std::string const resN = "__dwiden_res_" + std::to_string(n);

	// __dwiden_len = extract_uint16(srcBytes, 0)
	auto lenExtract = awst::makeExtractUInt16(
		_mkSourceBytes(), awst::makeIntegerConstant(0, _loc), _loc, u64);
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(lenN, u64, _loc), std::move(lenExtract), _loc));

	// __dwiden_i = 0
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::makeIntegerConstant(0, _loc), _loc));

	// __dwiden_res = extract3(itob(len), 6, 2)  // 2-byte length prefix
	auto itobLen = awst::makeItob(
		awst::makeVarExpression(lenN, u64, _loc), _loc);
	auto lenPrefix = awst::makeExtract3(std::move(itobLen),
		awst::makeIntegerConstant(6, _loc),
		awst::makeIntegerConstant(2, _loc), _loc);
	_emit(awst::makeAssignmentStatement(
		awst::makeVarExpression(resN, bytesT, _loc), std::move(lenPrefix), _loc));

	// Loop body
	auto body = awst::makeBlock(_loc);

	// offset = 2 + __dwiden_i * srcBytes
	auto idxRead = awst::makeVarExpression(idxN, u64, _loc);
	auto offMul = awst::makeUInt64BinOp(std::move(idxRead),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(srcBytes, _loc), _loc);
	auto offsetExpr = awst::makeUInt64BinOp(
		awst::makeIntegerConstant(2, _loc),
		awst::UInt64BinaryOperator::Add,
		std::move(offMul), _loc);

	// srcByte = extract3(srcBytes, offset, srcBytes)
	auto srcByte = awst::makeExtract3(_mkSourceBytes(),
		std::move(offsetExpr),
		awst::makeIntegerConstant(srcBytes, _loc), _loc);

	std::shared_ptr<awst::Expression> widened;
	if (isSigned)
	{
		auto signByte = awst::makeExtract3(srcByte,
			awst::makeIntegerConstant(0, _loc),
			awst::makeIntegerConstant(1, _loc), _loc);
		auto signByteVal = awst::makeBtoi(std::move(signByte), _loc);
		auto isNeg = awst::makeNumericCompare(
			std::move(signByteVal),
			awst::NumericComparison::Gte,
			awst::makeIntegerConstant(128, _loc), _loc);
		std::vector<uint8_t> ffPad(padBytes, 0xFFu);
		std::vector<uint8_t> zeroPad(padBytes, 0x00u);
		auto prepend = awst::makeConditional(
			std::move(isNeg),
			awst::makeBytesConstant(std::move(ffPad), _loc),
			awst::makeBytesConstant(std::move(zeroPad), _loc),
			bytesT, _loc);
		widened = awst::makeConcat(std::move(prepend), std::move(srcByte), _loc);
	}
	else
	{
		widened = awst::makeConcat(
			awst::makeBzero(padBytes, _loc),
			std::move(srcByte), _loc);
	}

	// __dwiden_res = concat(__dwiden_res, widened)
	auto resAppend = awst::makeConcat(
		awst::makeVarExpression(resN, bytesT, _loc),
		std::move(widened), _loc);
	body->body.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(resN, bytesT, _loc),
		std::move(resAppend), _loc));

	// __dwiden_i = __dwiden_i + 1
	auto idxInc = awst::makeUInt64BinOp(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(1, _loc), _loc);
	body->body.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(idxN, u64, _loc),
		std::move(idxInc), _loc));

	// while __dwiden_i < __dwiden_len { body }
	auto loopCond = awst::makeNumericCompare(
		awst::makeVarExpression(idxN, u64, _loc),
		awst::NumericComparison::Lt,
		awst::makeVarExpression(lenN, u64, _loc), _loc);
	_emit(awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	return awst::makeReinterpretCast(
		awst::makeVarExpression(resN, bytesT, _loc),
		_targetType, _loc);
}

} // namespace puyasol::builder
