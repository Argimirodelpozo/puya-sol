#include "builder/codec/ByteSlice.h"
#include "builder/BuildArtifacts.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> readPaddedBytes(
	TypeMapper& types,
	std::shared_ptr<awst::Expression> _bytes,
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _length, awst::SourceLocation const& _loc)
{
	using O = awst::UInt64BinaryOperator;
	// This checked/padded slice is used at every calldata load and precompile
	// ingress. Keep its general bounds logic in one generated helper.
	std::string const id = "__puyasol_read_padded_bytes";
	auto call = awst::makeSubroutineCall(awst::SubroutineID{id}, awst::WType::bytesType(), _loc);
	awst::pushCallArg(call->args, std::move(_bytes));
	awst::pushCallArg(call->args, TypeCoercion::implicitNumericCast(std::move(_offset), awst::WType::biguintType(), _loc));
	awst::pushCallArg(call->args, std::move(_length));
	auto& subs = types.artifacts().bufferSubroutines;
	if (subs.count(id)) return call;
	auto bytes = awst::makeVarExpression("bytes", awst::WType::bytesType(), _loc);
	auto len = awst::makeVarExpression("length", awst::WType::uint64Type(), _loc);
	auto off = awst::makeVarExpression("offset", awst::WType::biguintType(), _loc);
	auto size = awst::makeLen(bytes, _loc);
	auto start = awst::makeEvalOnce(awst::makeConditional(
		awst::makeNumericCompare(off, awst::NumericComparison::Lt, awst::makeAsBiguint(awst::makeItob(size, _loc), _loc), _loc),
		awst::makeBiguintToUInt64(off, _loc), size, awst::WType::uint64Type(), _loc), _loc);
	auto available = awst::makeUInt64BinOp(size, O::Sub, start, _loc);
	auto count = awst::makeEvalOnce(awst::makeConditional(
		awst::makeNumericCompare(len, awst::NumericComparison::Lt, available, _loc),
		len, available, awst::WType::uint64Type(), _loc), _loc);
	auto result = awst::makeConcat(awst::makeExtract3(bytes, start, count, _loc),
		awst::makeBzero(awst::makeUInt64BinOp(len, O::Sub, count, _loc), _loc), _loc);
	auto body = awst::makeBlock(_loc);
	body->body.push_back(awst::makeReturnStatement(std::move(result), _loc));
	auto sub = awst::makeSubroutine(id, id,
		{{"bytes", awst::WType::bytesType(), _loc}, {"offset", awst::WType::biguintType(), _loc},
			{"length", awst::WType::uint64Type(), _loc}},
		awst::WType::bytesType(), std::move(body), true, _loc);
	sub->inlineOpt = false;
	subs.emplace(id, std::move(sub));
	return call;
}

} // namespace puyasol::builder
