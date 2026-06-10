/// @file ArithmeticOps.cpp
/// Arithmetic and comparison operations: add, mul, mod, sub, eq, lt, gt, and, or, not, xor.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <string>

namespace puyasol::builder
{

// ─── Shared helpers ─────────────────────────────────────────────────────────

bool AssemblyBuilder::checkArity(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	size_t _n, char const* _name, awst::SourceLocation const& _loc,
	char const* _hint
)
{
	if (_args.size() != _n)
	{
		Logger::instance().error(
			std::string(_name) + " requires " + std::to_string(_n)
			+ (_n == 1 ? " argument" : " arguments")
			+ (_hint ? std::string(" (") + _hint + ")" : std::string()), _loc);
		return false;
	}
	return true;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::makeYulCompare(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::NumericComparison _cmp, char const* _name,
	awst::SourceLocation const& _loc
)
{
	// eq/lt/gt: coerce both operands to biguint and compare; result is a bool.
	if (!checkArity(_args, 2, _name, _loc))
		return nullptr;
	return awst::makeNumericCompare(
		ensureBiguint(_args[0], _loc), _cmp, ensureBiguint(_args[1], _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::makeYulBitwise(
	char const* _op,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	char const* _name, awst::SourceLocation const& _loc
)
{
	// and/or/xor: byte-wise opcode on both operands (coerced biguint→bytes),
	// reinterpreting the result back to biguint. No 32-byte padding is needed —
	// the missing high bytes of a shorter minimal-encoded operand act as zeros,
	// which is the correct result for &, | and ^ (only `not` must pad to 32).
	if (!checkArity(_args, 2, _name, _loc))
		return nullptr;
	auto call = awst::makeIntrinsicCall(_op, awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(awst::makeAsBytes(ensureBiguint(_args[0], _loc), _loc));
	call->stackArgs.push_back(awst::makeAsBytes(ensureBiguint(_args[1], _loc), _loc));
	return awst::makeAsBiguint(std::move(call), _loc);
}

// ─── Modular arithmetic ─────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMulmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// mulmod(a, b, c) = (a * b) % c, with the product computed in full precision
	// (no 2^256 wrap — that is the defining property of EVM mulmod). EVM defines
	// mulmod(a, b, 0) = 0; safeDivMod guards the AVM divide-by-zero panic.
	if (!checkArity(_args, 3, "mulmod", _loc))
		return nullptr;
	auto product = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Mult, _args[1], _loc
	);
	return safeDivMod(
		std::move(product), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAddmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// addmod(a, b, c) = (a + b) % c, with the sum computed in full precision
	// (no 2^256 wrap). EVM defines addmod(a, b, 0) = 0; safeDivMod guards the
	// AVM divide-by-zero panic.
	if (!checkArity(_args, 3, "addmod", _loc))
		return nullptr;
	auto sum = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, _args[1], _loc
	);
	return safeDivMod(
		std::move(sum), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAdd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "add", _loc))
		return nullptr;
	// EVM add wraps modulo 2^256
	auto sum = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, _args[1], _loc
	);
	return wrapMod256(std::move(sum), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMul(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "mul", _loc))
		return nullptr;
	// EVM mul wraps modulo 2^256
	auto product = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Mult, _args[1], _loc
	);
	return wrapMod256(std::move(product), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "mod", _loc))
		return nullptr;
	// EVM: mod(a, 0) = 0. AVM: b% by 0 panics.
	// Emit: b != 0 ? a % b : 0
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::Mod, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSub(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "sub", _loc))
		return nullptr;
	// EVM sub wraps modulo 2^256: result = (a + 2^256 - b) mod 2^256
	// This avoids AVM biguint underflow when a < b
	auto aPlusPow = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, makeTwoPow256(_loc), _loc
	);
	auto diff = makeBigUIntBinOp(
		std::move(aPlusPow), awst::BigUIntBinaryOperator::Sub, _args[1], _loc
	);
	return wrapMod256(std::move(diff), _loc);
}

// ─── Comparisons ────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleIszero(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "iszero", _loc))
		return nullptr;
	// iszero(x): if x is already bool, emit Not; otherwise x == 0
	if (_args[0]->wtype == awst::WType::boolType())
		return awst::makeNot(_args[0], _loc);

	auto arg = ensureBiguint(_args[0], _loc);
	auto zero = awst::makeBiguintConstant("0", _loc);
	return awst::makeNumericCompare(
		std::move(arg), awst::NumericComparison::Eq, std::move(zero), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleEq(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulCompare(_args, awst::NumericComparison::Eq, "eq", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleLt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulCompare(_args, awst::NumericComparison::Lt, "lt", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulCompare(_args, awst::NumericComparison::Gt, "gt", _loc);
}

// ─── Bitwise ────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAnd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b&", _args, "and", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleOr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b|", _args, "or", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleXor(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b^", _args, "xor", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleNot(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "not", _loc))
		return nullptr;
	// EVM `not` operates on 256-bit values. AVM `b~` operates on actual byte length.
	// Pad input to 32 bytes so b~ produces a 256-bit result (e.g. not(0) = MAX_UINT256).
	auto padded = padTo32Bytes(ensureBiguint(_args[0], _loc), _loc);
	auto call = awst::makeIntrinsicCall("b~", awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(padded));
	return awst::makeAsBiguint(std::move(call), _loc);
}

} // namespace puyasol::builder
