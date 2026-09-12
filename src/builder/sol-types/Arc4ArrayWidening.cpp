#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "awst/NameGen.h"

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (_value->wtype != awst::WType::uint64Type()) return nullptr;
	auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(_targetType);
	if (!arc4 || arc4->n() >= 64 || arc4->n() % 8 != 0) return nullptr;
	int const bytes = arc4->n() / 8;
	return awst::makeReinterpretCast(
		awst::makeExtract(awst::makeItob(std::move(_value), _loc), 8 - bytes, bytes, _loc),
		_targetType, _loc);
}

std::shared_ptr<awst::Expression> tryConvertArc4Array(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	std::vector<std::shared_ptr<awst::Statement>>* _pre,
	awst::SourceLocation const& _loc)
{
	auto const* ss = dynamic_cast<awst::ARC4StaticArray const*>(_value->wtype);
	auto const* sd = dynamic_cast<awst::ARC4DynamicArray const*>(_value->wtype);
	auto const* ts = dynamic_cast<awst::ARC4StaticArray const*>(_targetType);
	auto const* td = dynamic_cast<awst::ARC4DynamicArray const*>(_targetType);
	if ((!ss && !sd) || (!ts && !td) || (sd && ts)) return nullptr;
	if (ss && ts && ss->arraySize() > ts->arraySize()) return nullptr;
	auto const* sourceElem = ss ? ss->elementType() : sd->elementType();
	auto const* targetElem = ts ? ts->elementType() : td->elementType();
	bool const sameElement = awst::structurallyEquivalent(sourceElem, targetElem);
	if (ss && td) checkedSize<uint16_t>(ss->arraySize(), "ARC4 array length");
	// ConvertArray understands packed bools; a byte stride does not.
	if (sameElement && ss && td)
		return awst::makeConvertArray(std::move(_value), _targetType, _loc);
	auto const sourceStride = computeEncodedElementSize(sourceElem).fixedBytes<int>();
	auto const targetStride = computeEncodedElementSize(targetElem).fixedBytes<int>();
	if (ss && ts && (!sourceStride || !targetStride) && ts->arraySize() <= 256)
	{
		// Dynamic elements need new head/tail offsets, not byte padding.
		// Puya's typed array encoder also owns packed-bool layout.
		auto source = awst::makeEvalOnce(std::move(_value), _loc);
		auto result = awst::makeNewArray(_targetType, _loc);
		for (int64_t i = 0; i < ts->arraySize(); ++i)
			result->values.push_back(i < ss->arraySize()
				? TypeCoercion::coerceForAssignment(awst::makeIndexExpression(source,
					awst::makeIntegerConstant(i, _loc), sourceElem, _loc), targetElem, _loc, _pre)
				: TypeCoercion::makeDefaultValue(targetElem, _loc));
		return result;
	}
	if (sameElement && ss && ts && sourceStride)
		return awst::makeReinterpretCast(awst::makeRightPad(
			awst::makeAsBytes(std::move(_value), _loc), EncodedSize::fixed(*sourceStride)
				.times(ts->arraySize() - ss->arraySize()).fixedBytes<int>().value(), _loc),
			_targetType, _loc);

	// Generic fixed copies recurse through the scalar/aggregate converter.
	// Dynamic sources use a stride loop only for same-signed integer widening,
	// whose legality is checked at the solc-typed ConversionPlan front door.
	auto const sourceInt = SolIntType::fromArc4(sourceElem);
	auto const targetInt = SolIntType::fromArc4(targetElem);
	bool const integerWiden = sourceInt && targetInt && sourceInt->bits < targetInt->bits
		&& sourceInt->isSigned == targetInt->isSigned;
	bool const unroll = ss && ss->arraySize() <= 256;
	if (!sourceStride || !targetStride || (!unroll && (!integerWiden || !_pre)))
		return nullptr;
	unsigned const stride = *sourceStride;
	auto convert = [&](std::shared_ptr<awst::Expression> bytes) {
		return awst::makeAsBytes(TypeCoercion::coerceForAssignment(
			awst::makeReinterpretCast(std::move(bytes), sourceElem, _loc), targetElem, _loc), _loc);
	};
	auto padTail = [&](std::shared_ptr<awst::Expression> bytes) {
		if (ss && ts && ts->arraySize() > ss->arraySize())
			bytes = awst::makeRightPad(std::move(bytes),
				EncodedSize::fixed(*targetStride).times(ts->arraySize() - ss->arraySize())
					.fixedBytes<int>().value(), _loc);
		return awst::makeReinterpretCast(std::move(bytes), _targetType, _loc);
	};
	if (unroll)
	{
		auto source = awst::makeEvalOnce(awst::makeAsBytes(std::move(_value), _loc), _loc);
		std::vector<uint8_t> header;
		if (td) header = {static_cast<uint8_t>(ss->arraySize() >> 8),
			static_cast<uint8_t>(ss->arraySize())};
		std::shared_ptr<awst::Expression> bytes = awst::makeBytesConstant(std::move(header), _loc);
		for (int64_t i = 0; i < ss->arraySize(); ++i)
			bytes = awst::makeConcat(std::move(bytes),
				convert(awst::makeExtract(source, checkedSize<int>(i * stride, "array element offset"), stride, _loc)), _loc);
		return padTail(std::move(bytes));
	}

	std::string const id = std::to_string(awst::NameGen::next("Arc4ArrayWidening"));
	auto bytesVar = [&](std::string const& suffix) {
		return awst::makeVarExpression("__widen_" + suffix + id, awst::WType::bytesType(), _loc);
	};
	auto index = [&]() {
		return awst::makeVarExpression("__widen_i" + id, awst::WType::uint64Type(), _loc);
	};
	_pre->push_back(awst::makeAssignmentStatement(
		bytesVar("src"), awst::makeAsBytes(std::move(_value), _loc), _loc));
	auto length = ss ? awst::makeIntegerConstant(ss->arraySize(), _loc)
		: std::shared_ptr<awst::Expression>{awst::makeExtractUInt16(bytesVar("src"), awst::makeZero(_loc), _loc)};
	std::shared_ptr<awst::Expression> header = td
		? awst::makeExtract(awst::makeItob(length, _loc), 6, 2, _loc)
		: std::shared_ptr<awst::Expression>{awst::makeBytesConstant({}, _loc)};
	_pre->push_back(awst::makeAssignmentStatement(bytesVar("out"), std::move(header), _loc));
	_pre->push_back(awst::makeAssignmentStatement(index(), awst::makeZero(_loc), _loc));
	auto body = awst::makeBlock(_loc);
	auto offset = awst::makeUInt64BinOp(index(), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(stride, _loc), _loc);
	if (sd) offset = awst::makeUInt64BinOp(std::move(offset), awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(2, _loc), _loc);
	body->body.push_back(awst::makeAssignmentStatement(bytesVar("out"),
		awst::makeConcat(bytesVar("out"), convert(awst::makeExtract3(
			bytesVar("src"), std::move(offset), awst::makeIntegerConstant(stride, _loc), _loc)), _loc), _loc));
	body->body.push_back(awst::makeAssignmentStatement(index(), awst::makeUInt64BinOp(
		index(), awst::UInt64BinaryOperator::Add, awst::makeOne(_loc), _loc), _loc));
	_pre->push_back(awst::makeWhileLoop(awst::makeNumericCompare(index(),
		awst::NumericComparison::Lt, std::move(length), _loc), std::move(body), _loc));
	return padTail(bytesVar("out"));
}

} // namespace puyasol::builder
