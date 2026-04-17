/// @file ArithmeticOps.cpp
/// Arithmetic and comparison operations: add, mul, mod, sub, eq, lt, gt, and, or, not, xor.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMulmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// mulmod(a, b, c) = (a * b) % c
	if (_args.size() != 3)
	{
		Logger::instance().error("mulmod requires 3 arguments", _loc);
		return nullptr;
	}
	auto product = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Mult, _args[1], _loc
	);
	return makeBigUIntBinOp(
		std::move(product), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAddmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// addmod(a, b, c) = (a + b) % c
	if (_args.size() != 3)
	{
		Logger::instance().error("addmod requires 3 arguments", _loc);
		return nullptr;
	}
	auto sum = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, _args[1], _loc
	);
	return makeBigUIntBinOp(
		std::move(sum), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAdd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("add requires 2 arguments", _loc);
		return nullptr;
	}
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
	if (_args.size() != 2)
	{
		Logger::instance().error("mul requires 2 arguments", _loc);
		return nullptr;
	}
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
	if (_args.size() != 2)
	{
		Logger::instance().error("mod requires 2 arguments", _loc);
		return nullptr;
	}
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
	if (_args.size() != 2)
	{
		Logger::instance().error("sub requires 2 arguments", _loc);
		return nullptr;
	}
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

std::shared_ptr<awst::Expression> AssemblyBuilder::handleIszero(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("iszero requires 1 argument", _loc);
		return nullptr;
	}
	// iszero(x): if x is already bool, emit Not; otherwise x == 0
	if (_args[0]->wtype == awst::WType::boolType())
	{
		auto notExpr = std::make_shared<awst::Not>();
		notExpr->sourceLocation = _loc;
		notExpr->wtype = awst::WType::boolType();
		notExpr->expr = _args[0];
		return notExpr;
	}

	auto arg = ensureBiguint(_args[0], _loc);

	auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = std::move(arg);
	cmp->op = awst::NumericComparison::Eq;
	cmp->rhs = std::move(zero);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleEq(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("eq requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Eq;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleLt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("lt requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Lt;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("gt requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Gt;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAnd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("and requires 2 arguments", _loc);
		return nullptr;
	}
	// Bitwise AND on biguint: use b& opcode
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b&";
	// Convert both operands to biguint first, then to bytes
	auto lhsCast = awst::makeReinterpretCast(ensureBiguint(_args[0], _loc), awst::WType::bytesType(), _loc);
	auto rhsCast = awst::makeReinterpretCast(ensureBiguint(_args[1], _loc), awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	// Reinterpret result back to biguint
	auto result = awst::makeReinterpretCast(std::move(call), awst::WType::biguintType(), _loc);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleOr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("or requires 2 arguments", _loc);
		return nullptr;
	}
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b|";
	auto lhsCast = awst::makeReinterpretCast(ensureBiguint(_args[0], _loc), awst::WType::bytesType(), _loc);
	auto rhsCast = awst::makeReinterpretCast(ensureBiguint(_args[1], _loc), awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	auto result = awst::makeReinterpretCast(std::move(call), awst::WType::biguintType(), _loc);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleNot(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("not requires 1 argument", _loc);
		return nullptr;
	}
	// EVM `not` operates on 256-bit values. AVM `b~` operates on actual byte length.
	// Pad input to 32 bytes so b~ produces a 256-bit result (e.g. not(0) = MAX_UINT256).
	auto padded = padTo32Bytes(ensureBiguint(_args[0], _loc), _loc);
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b~";
	call->stackArgs.push_back(std::move(padded));
	auto result = awst::makeReinterpretCast(std::move(call), awst::WType::biguintType(), _loc);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleXor(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("xor requires 2 arguments", _loc);
		return nullptr;
	}
	// Bitwise XOR on biguint: use b^ opcode
	// Coerce bool operands to biguint first (Yul: all values are uint256)
	auto lhs = ensureBiguint(_args[0], _loc);
	auto rhs = ensureBiguint(_args[1], _loc);

	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b^";
	// Convert both operands to bytes first
	auto lhsCast = awst::makeReinterpretCast(std::move(lhs), awst::WType::bytesType(), _loc);
	auto rhsCast = awst::makeReinterpretCast(std::move(rhs), awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	// Reinterpret result back to biguint
	auto result = awst::makeReinterpretCast(std::move(call), awst::WType::biguintType(), _loc);
	return result;
}

} // namespace puyasol::builder
