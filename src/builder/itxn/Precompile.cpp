/// @file Precompile.cpp
/// Precompile algorithms consume normalized bytes and return complete result bytes.

#include "builder/itxn/Precompile.h"
#include "builder/codec/ByteSlice.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"
#include "builder/SecpRangeCheck.h"
#include "awst/NameGen.h"

namespace puyasol::builder
{
namespace
{

std::shared_ptr<awst::Expression> ecRecover(
	TypeMapper& types,
	std::shared_ptr<awst::Expression> _input, awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto input = awst::makeEvalOnce(readPaddedBytes(types, std::move(_input), awst::makeZero(_loc),
		awst::makeIntegerConstant("128", _loc), _loc), _loc);
	auto v = awst::makeAsBiguint(awst::makeExtract(input, 32, 32, _loc), _loc);
	auto r = [&] { return awst::makeExtract(input, 64, 32, _loc); };
	auto s = [&] { return awst::makeExtract(input, 96, 32, _loc); };
	auto valid = awst::makeBoolBinOp(
		awst::makeBoolBinOp(
			awst::makeNumericCompare(v, awst::NumericComparison::Eq,
				awst::makeBiguintConstant("27", _loc), _loc),
			awst::BinaryBooleanOperator::Or,
			awst::makeNumericCompare(v, awst::NumericComparison::Eq,
				awst::makeBiguintConstant("28", _loc), _loc), _loc),
		awst::BinaryBooleanOperator::And, secp256k1RangeCondition(r, s, _loc), _loc);

	auto suffix = std::to_string(awst::NameGen::next("Precompile.ecrecover"));
	auto result = awst::makeVarExpression("__ecrecover_" + suffix, awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(result, awst::makeBytesConstant({}, _loc), _loc));
	auto block = awst::makeBlock(_loc);
	auto tupleType = types.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()});
	auto recovered = awst::makeVarExpression("__ecrecover_key_" + suffix, tupleType, _loc);
	auto call = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleType, _loc);
	call->immediates = {"Secp256k1"};
	call->stackArgs = {awst::makeExtract(input, 0, 32, _loc),
		awst::makeBiguintToUInt64(awst::makeBigUIntBinOp(v, awst::BigUIntBinaryOperator::Sub,
			awst::makeBiguintConstant("27", _loc), _loc), _loc), r(), s()};
	block->body.push_back(awst::makeAssignmentStatement(recovered, std::move(call), _loc));
	auto hash = awst::makeKeccak256(awst::makeConcat(
		awst::makeTupleItem(recovered, 0, awst::WType::bytesType(), _loc),
		awst::makeTupleItem(recovered, 1, awst::WType::bytesType(), _loc), _loc), _loc);
	block->body.push_back(awst::makeAssignmentStatement(result,
		awst::makeLeftPad(awst::makeExtract(std::move(hash), 12, 20, _loc), 12, _loc), _loc));
	// Invalid v/r/s succeeds with empty return data, leaving output memory intact.
	_out.push_back(awst::makeIfElse(std::move(valid), std::move(block), nullptr, _loc));
	return result;
}

std::shared_ptr<awst::Expression> ecPairing(
	std::shared_ptr<awst::Expression> _input, awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	using O = awst::UInt64BinaryOperator;
	auto input = awst::makeEvalOnce(std::move(_input), _loc);
	auto length = awst::makeLen(input, _loc);
	_out.push_back(awst::makeExpressionStatement(awst::makeAssert(
		awst::makeNumericCompare(awst::makeUInt64BinOp(length, O::Mod,
			awst::makeIntegerConstant("192", _loc), _loc), awst::NumericComparison::Eq,
			awst::makeZero(_loc), _loc), _loc, "pairing input must contain complete 192-byte pairs"), _loc));

	auto suffix = std::to_string(awst::NameGen::next("Precompile.pairing"));
	auto offset = awst::makeVarExpression("__pairing_offset_" + suffix, awst::WType::uint64Type(), _loc);
	auto g1 = awst::makeVarExpression("__pairing_g1_" + suffix, awst::WType::bytesType(), _loc);
	auto g2 = awst::makeVarExpression("__pairing_g2_" + suffix, awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(offset, awst::makeZero(_loc), _loc));
	for (auto const& points: {g1, g2})
		_out.push_back(awst::makeAssignmentStatement(points, awst::makeBytesConstant({}, _loc), _loc));
	auto slice = [&](uint64_t start, uint64_t size) {
		return awst::makeExtract3(input, awst::makeUInt64BinOp(offset, O::Add,
			awst::makeIntegerConstant(start, _loc), _loc), awst::makeIntegerConstant(size, _loc), _loc);
	};
	auto body = awst::makeBlock(_loc);
	body->body.push_back(awst::makeAssignmentStatement(g1, awst::makeConcat(g1, slice(0, 64), _loc), _loc));
	// EVM imaginary/real coordinate order -> AVM real/imaginary order.
	auto pair = awst::makeConcat(awst::makeConcat(slice(96, 32), slice(64, 32), _loc),
		awst::makeConcat(slice(160, 32), slice(128, 32), _loc), _loc);
	body->body.push_back(awst::makeAssignmentStatement(g2, awst::makeConcat(g2, std::move(pair), _loc), _loc));
	body->body.push_back(awst::makeAssignmentStatement(offset, awst::makeUInt64BinOp(
		offset, O::Add, awst::makeIntegerConstant("192", _loc), _loc), _loc));
	_out.push_back(awst::makeWhileLoop(awst::makeNumericCompare(
		offset, awst::NumericComparison::Lt, length, _loc), std::move(body), _loc));
	auto check = awst::makeIntrinsicCall("ec_pairing_check", awst::WType::boolType(), _loc);
	check->immediates = {"BN254g1"};
	check->stackArgs = {g1, g2};
	auto valid = awst::makeConditional(awst::makeNumericCompare(length, awst::NumericComparison::Eq,
		awst::makeZero(_loc), _loc), awst::makeTrue(_loc), std::move(check), awst::WType::boolType(), _loc);
	return awst::makeLeftPad(awst::makeItob(awst::makeAsUInt64(std::move(valid), _loc), _loc), 24, _loc);
}

std::shared_ptr<awst::Expression> modExp(
	TypeMapper& types,
	std::shared_ptr<awst::Expression> _input, awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto input = awst::makeEvalOnce(readPaddedBytes(types, std::move(_input), awst::makeZero(_loc),
		awst::makeIntegerConstant("192", _loc), _loc), _loc);
	auto word = [&](uint64_t offset) {
		return awst::makeAsBiguint(awst::makeExtract(input, offset, 32, _loc), _loc);
	};
	// The supported shape is still 32/32/32; never silently reinterpret a
	// different EIP-198 layout as three fixed-width operands.
	for (uint64_t offset: {0, 32, 64})
		_out.push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeNumericCompare(word(offset), awst::NumericComparison::Eq,
				awst::makeBiguintConstant("32", _loc), _loc), _loc,
			"modexp precompile only supports 32-byte operands"), _loc));
	auto suffix = std::to_string(awst::NameGen::next("Precompile.modexp"));
	auto var = [&](std::string name) {
		return awst::makeVarExpression("__modexp_" + name + "_" + suffix, awst::WType::biguintType(), _loc);
	};
	auto result = var("result"), base = var("base"), exp = var("exp"), mod = var("mod");
	auto constant = [&](char const* value) { return awst::makeBiguintConstant(value, _loc); };
	auto op = [&](auto left, awst::BigUIntBinaryOperator kind, auto right) {
		return awst::makeBigUIntBinOp(std::move(left), kind, std::move(right), _loc);
	};
	using O = awst::BigUIntBinaryOperator;
	_out.push_back(awst::makeAssignmentStatement(mod, word(160), _loc));
	_out.push_back(awst::makeAssignmentStatement(result, constant("0"), _loc));
	auto nonZero = awst::makeBlock(_loc);
	nonZero->body.push_back(awst::makeAssignmentStatement(result, op(constant("1"), O::Mod, mod), _loc));
	nonZero->body.push_back(awst::makeAssignmentStatement(base, op(word(96), O::Mod, mod), _loc));
	nonZero->body.push_back(awst::makeAssignmentStatement(exp, word(128), _loc));
	auto body = awst::makeBlock(_loc), odd = awst::makeBlock(_loc);
	odd->body.push_back(awst::makeAssignmentStatement(result,
		op(op(result, O::Mult, base), O::Mod, mod), _loc));
	body->body.push_back(awst::makeIfElse(awst::makeNumericCompare(
		op(exp, O::BitAnd, constant("1")), awst::NumericComparison::Ne, constant("0"), _loc),
		std::move(odd), nullptr, _loc));
	body->body.push_back(awst::makeAssignmentStatement(exp, op(exp, O::FloorDiv, constant("2")), _loc));
	body->body.push_back(awst::makeAssignmentStatement(base, op(op(base, O::Mult, base), O::Mod, mod), _loc));
	nonZero->body.push_back(awst::makeWhileLoop(awst::makeNumericCompare(
		exp, awst::NumericComparison::Gt, constant("0"), _loc), std::move(body), _loc));
	_out.push_back(awst::makeIfElse(awst::makeNumericCompare(
		mod, awst::NumericComparison::Ne, constant("0"), _loc), std::move(nonZero), nullptr, _loc));
	return awst::makeExtractLastN(awst::makeLeftPad(awst::makeAsBytes(result, _loc), 32, _loc), 32, _loc);
}

} // namespace

std::shared_ptr<awst::Expression> evaluatePrecompile(
	TypeMapper& types, uint64_t address, std::shared_ptr<awst::Expression> input,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out)
{
	input = awst::makeEvalOnce(std::move(input), loc);
	switch (address)
	{
	case 1: return ecRecover(types, std::move(input), loc, out);
	case 2: return awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), loc, {}, {input});
	case 4: return input;
	case 5: return modExp(types, std::move(input), loc, out);
	case 6:
	case 7:
	{
		auto bytes = awst::makeEvalOnce(readPaddedBytes(types, std::move(input), awst::makeZero(loc),
			awst::makeIntegerConstant(address == 6 ? "128" : "96", loc), loc), loc);
		auto ec = awst::makeIntrinsicCall(address == 6 ? "ec_add" : "ec_scalar_mul",
			awst::WType::bytesType(), loc);
		ec->immediates = {"BN254g1"};
		ec->stackArgs = {awst::makeExtract(bytes, 0, 64, loc),
			awst::makeExtract(bytes, 64, address == 6 ? 64 : 32, loc)};
		return ec;
	}
	case 8: return ecPairing(std::move(input), loc, out);
	default:
		Logger::instance().error("EVM precompile " + std::to_string(address)
			+ " has no supported AVM implementation", loc);
		return nullptr;
	}
}

} // namespace puyasol::builder
