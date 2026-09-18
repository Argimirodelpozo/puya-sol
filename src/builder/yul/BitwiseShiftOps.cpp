/// @file BitwiseShiftOps.cpp
/// Bitwise and shift operations: shl, shr, div, byte, signextend.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/eb/BigUIntMathHelpers.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/storage/StorageMapper.h"

#include <libsolutil/Numeric.h>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::tryRouteConstSlotLoad(
	std::shared_ptr<awst::Expression> const& _slot,
	awst::SourceLocation const& _loc)
{
	auto const* constant = dynamic_cast<awst::IntegerConstant const*>(_slot.get());
	if (!constant) return nullptr;
	auto found = m_context->slotRoutes.find(constant->value);
	if (found == m_context->slotRoutes.end()) return nullptr;
	auto const& route = found->second;
	auto key = awst::makeUtf8BytesConstant(route.varName, _loc, awst::WType::stateKeyType());
	auto value = StorageMapper::makeStateGetWithDefault(
		awst::makeAppStateExpression(std::move(key), awst::WType::bytesType(), _loc),
		awst::WType::bytesType(), _loc);
	return awst::makeAsBiguint(awst::makeExtractLastN(
		awst::makeLeftPad(std::move(value), 32, _loc), 32, _loc), _loc);
}

bool AssemblyBuilder::tryRouteConstSlotStore(
	std::shared_ptr<awst::Expression> const& _slot,
	std::shared_ptr<awst::Expression> const& _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto const* constant = dynamic_cast<awst::IntegerConstant const*>(_slot.get());
	if (!constant) return false;
	auto found = m_context->slotRoutes.find(constant->value);
	if (found == m_context->slotRoutes.end()) return false;
	auto const& route = found->second;
	auto key = awst::makeUtf8BytesConstant(route.varName, _loc, awst::WType::stateKeyType());
	auto target = awst::makeAppStateExpression(std::move(key), route.wtype, _loc);
	_out.push_back(awst::makeExpressionStatement(awst::makeAssignmentExpression(
		std::move(target), ensureBiguint(_value, _loc), _loc, route.wtype), _loc));
	return true;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGas(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces via ensureBiguint (match at consumption, drops itob widen).
	EvmFeaturePolicy::report(
		EvmFeature::GasLeft, m_typeMapper.profile(), _loc);
	return awst::makeGlobal(std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces (same natural-type convention as gas/number/selfbalance).
	return awst::makeGlobal("LatestTimestamp", awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// EVM div(a,0)=0; AVM panics.
	return safeDivMod(
		wrapMod256(ensureBiguint(_args[0], _loc), _loc),
		awst::BigUIntBinaryOperator::FloorDiv,
		wrapMod256(ensureBiguint(_args[1], _loc), _loc), _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildLogicalShift(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	bool _left, awst::SourceLocation const& _loc
)
{
	// Operand evaluation is already sequenced by buildOperands. Keep Yul's
	// aggregate-pointer conversion and word normalization at this boundary.
	return eb::buildBigUIntShift(wrapMod256(ensureBiguint(_args[1], _loc), _loc),
		eb::shiftAmountToUint64(ensureBiguint(_args[0], _loc), _loc), _left, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return buildLogicalShift(_args, /*_left=*/true, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return buildLogicalShift(_args, /*_left=*/false, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleByte(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// byte(n, x): extract byte n from 32-byte big-endian x → biguint.

	auto padded = padTo32Bytes(_args[1], _loc);

	// n >= 32: EVM byte() returns 0 (out of range). Range-check the ORIGINAL n as a
	// biguint — checking the btoi-truncated value is wrong: a huge n (>= 2^64)
	// truncates to a small in-range index and wrongly extracts a byte (found
	// fuzzing Solady DateTimeLib.daysInMonth: byte(2^128+5, ...) returned 31, not
	// 0). The conditional only evaluates the extract on the taken branch (n < 32),
	// so the btoi used there is always in range and never OOB-reverts.
	auto nBig = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	auto inRange = awst::makeNumericCompare(
		nBig, awst::NumericComparison::Lt,
		awst::makeIntegerConstant("32", _loc, awst::WType::biguintType()), _loc);
	auto nU64 = safeBtoi(nBig, _loc);
	auto extracted = awst::makeAsBiguint(
		awst::makeExtract3(std::move(padded), std::move(nU64), awst::makeOne(_loc), _loc), _loc);
	return awst::makeConditional(
		std::move(inRange), std::move(extracted),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	auto x = ensureBiguint(_args[1], _loc);
	std::shared_ptr<awst::Expression> shift;
	if (auto const* literal = dynamic_cast<awst::IntegerConstant const*>(_args[0].get()))
	{
		// Yul constants are full EVM words. Check the no-op range BEFORE narrowing.
		auto const byte = solidity::u256(literal->value);
		if (byte >= 31) return x;
		shift = awst::makeIntegerConstant(uint64_t{248} - 8 * static_cast<uint64_t>(byte), _loc);
	}
	else
	{
		auto byte = awst::makeEvalOnce(eb::shiftAmountToUint64(ensureBiguint(_args[0], _loc), _loc), _loc);
		shift = awst::makeConditional(awst::makeNumericCompare(byte, awst::NumericComparison::Lt,
			awst::makeIntegerConstant(31, _loc), _loc),
			awst::makeUInt64BinOp(awst::makeIntegerConstant(248, _loc), awst::UInt64BinaryOperator::Sub,
				awst::makeUInt64BinOp(byte, awst::UInt64BinaryOperator::Mult, awst::makeIntegerConstant(8, _loc), _loc), _loc),
			awst::makeZero(_loc), awst::WType::uint64Type(), _loc);
	}
	// signextend(b, x) = sar(s, shl(s, x)), s = 248 - 8*min(b, 31).
	shift = awst::makeEvalOnce(std::move(shift), _loc);
	return eb::buildBigUIntArithmeticShiftRight(eb::buildBigUIntShift(
		wrapMod256(std::move(x), _loc), shift, true, _loc), shift, _loc);
}


} // namespace puyasol::builder
