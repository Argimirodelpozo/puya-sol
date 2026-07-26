/// @file ArithmeticOps.cpp
/// Arithmetic and comparison operations: add, mul, mod, sub, eq, lt, gt, and, or, not, xor.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <boost/multiprecision/cpp_int.hpp>
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
	// No 32-byte padding needed — missing high bytes of minimal-encoded operands act as
	// zeros, giving the right result for &, |, ^ (only `not` must pad to 32).
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
	// mulmod(a,b,c) = (a*b)%c in full precision (no 2^256 wrap). EVM defines mulmod(a,b,0)=0;
	// safeDivMod guards the AVM divide-by-zero panic.
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
	// addmod(a,b,c) = (a+b)%c in full precision (no 2^256 wrap). EVM defines addmod(a,b,0)=0;
	// safeDivMod guards the AVM divide-by-zero panic.
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

std::shared_ptr<awst::Expression> AssemblyBuilder::handleExp(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "exp", _loc))
		return nullptr;
	// AVM has no exp opcode. Fold when BOTH operands are compile-time constants
	// (the idiomatic Yul use is byte-shifting by a power of a literal, e.g.
	// `exp(256, 12)` = 2^96 in ENS AddrResolver's asm addr<->bytes). EVM exp
	// wraps mod 2^256; compute via modular exponentiation to avoid huge
	// intermediates. Non-constant exponents stay a hard error (no silent 0).
	auto const* baseC = dynamic_cast<awst::IntegerConstant const*>(_args[0].get());
	auto const* expC = dynamic_cast<awst::IntegerConstant const*>(_args[1].get());
	if (baseC && expC)
	{
		using boost::multiprecision::cpp_int;
		cpp_int const mod = cpp_int(1) << 256;
		cpp_int base(baseC->value);
		cpp_int e(expC->value);
		base %= mod;
		if (base < 0) base += mod;
		cpp_int result = 1;
		while (e > 0)
		{
			if ((e & 1) != 0) result = (result * base) % mod;
			base = (base * base) % mod;
			e >>= 1;
		}
		return awst::makeIntegerConstant(result.str(), _loc, awst::WType::biguintType());
	}
	Logger::instance().error(
		"unsupported Yul builtin `exp` with a non-constant operand: no AVM exp "
		"opcode exists (only compile-time-constant exponentiation is folded).", _loc);
	return awst::makeZero(_loc, awst::WType::biguintType());
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
	// EVM sub wraps mod 2^256: (a + 2^256 - b) mod 2^256. Avoids AVM biguint underflow when a < b.
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
	// AVM `b~` operates on actual byte length; pad to 32 so b~ gives the 256-bit result (not(0) = MAX_UINT256).
	auto padded = padTo32Bytes(ensureBiguint(_args[0], _loc), _loc);
	auto call = awst::makeIntrinsicCall("b~", awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(padded));
	return awst::makeAsBiguint(std::move(call), _loc);
}

} // namespace puyasol::builder
