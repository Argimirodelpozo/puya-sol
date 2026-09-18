/// @file ArithmeticOps.cpp
/// Arithmetic and comparison operations: add, mul, mod, sub, eq, lt, gt, and, or, not, xor.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/eb/BigUIntMathHelpers.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <string>

namespace puyasol::builder
{

// ─── Shared helpers ─────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::makeYulCompare(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::NumericComparison _cmp,
	awst::SourceLocation const& _loc
)
{
	return awst::makeNumericCompare(
		ensureBiguint(_args[0], _loc), _cmp, ensureBiguint(_args[1], _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::makeYulBitwise(
	char const* _op,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// No 32-byte padding needed — missing high bytes of minimal-encoded operands act as
	// zeros, giving the right result for &, |, ^ (only `not` must pad to 32).
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
	// AVM has no exp opcode. Fold when BOTH operands are compile-time constants
	// (the idiomatic Yul use is byte-shifting by a power of a literal, e.g.
	// `exp(256, 12)` = 2^96 in ENS AddrResolver's asm addr<->bytes). EVM exp
	// wraps mod 2^256; compute via modular exponentiation to avoid huge
	// intermediates.
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
	// Runtime operands: square-and-multiply, wrapping mod 2^256 like the EVM
	// opcode (0**0 = 1 falls out of the loop shape). Same helper the
	// Solidity-level unchecked `**` uses; the loop lands in the pending
	// statements the enclosing statement handler drains.
	return eb::buildBigUIntExpInto(
		m_frame.pendingStatements, /*_isUnchecked=*/true,
		ensureBiguint(_args[0], _loc), ensureBiguint(_args[1], _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
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
	return makeYulCompare(_args, awst::NumericComparison::Eq, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleLt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulCompare(_args, awst::NumericComparison::Lt, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulCompare(_args, awst::NumericComparison::Gt, _loc);
}

// ─── Bitwise ────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleAnd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b&", _args, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleOr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b|", _args, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleXor(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return makeYulBitwise("b^", _args, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleNot(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// AVM `b~` operates on actual byte length; pad to 32 so b~ gives the 256-bit result (not(0) = MAX_UINT256).
	auto padded = padTo32Bytes(ensureBiguint(_args[0], _loc), _loc);
	auto call = awst::makeIntrinsicCall("b~", awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(padded));
	return awst::makeAsBiguint(std::move(call), _loc);
}

} // namespace puyasol::builder
