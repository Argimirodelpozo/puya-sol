/// @file SolcConstFold.cpp
/// See SolcConstFold.h — the one place builders get compile-time constants.

#include "builder/sol-types/SolcConstFold.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/analysis/ConstantEvaluator.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

using namespace solidity::frontend;

namespace
{

/// tryEvaluate the node to an integral rational; nullopt on anything else.
/// tryEvaluate is silent on failure (local discarded ErrorReporter + FatalError
/// catch), so speculative calls here can never emit solc diagnostics.
std::optional<rational> nodeValue(Expression const& _e)
{
	auto tv = ConstantEvaluator::tryEvaluate(_e);
	if (!std::holds_alternative<rational>(tv.value))
		return std::nullopt;
	auto const& rat = std::get<rational>(tv.value);
	if (rat.denominator() != 1)
		return std::nullopt;
	return rat;
}

/// The load-bearing guard of foldTyped: TRUE iff every integer-typed node in
/// the subtree evaluates in range of its OWN annotated type (rational-typed
/// nodes are solc-exact leaves and pass through). Any out-of-range
/// intermediate, unsupported node kind, or evaluator failure rejects the
/// whole fold — rejection is always safe (the normal lowering runs).
bool subtreeFoldable(Expression const& _e)
{
	auto const* type = _e.annotation().type;
	if (!type)
		return false;

	// solc already folded this subtree to an exact rational (literal
	// arithmetic, type(T).min/max, ...) — participates exactly; the integer
	// PARENT's own range check bounds whatever it combines into.
	if (dynamic_cast<RationalNumberType const*>(type))
		return true;

	auto const* intType = dynamic_cast<IntegerType const*>(type);
	if (!intType)
		return false;
	auto value = nodeValue(_e);
	if (!value)
		return false;
	if (value->numerator() < intType->minValue() || value->numerator() > intType->maxValue())
		return false;

	if (dynamic_cast<Literal const*>(&_e))
		return true;
	if (auto const* id = dynamic_cast<Identifier const*>(&_e))
	{
		// A constant variable's VALUE (computed + range-checked above) is what
		// solc itself inlines at references — its initializer's internals are
		// solc's business, not re-validated here.
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
			id->annotation().referencedDeclaration);
		return varDecl && varDecl->isConstant();
	}
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&_e))
	{
		// Parenthesized expression only.
		if (tuple->components().size() != 1 || !tuple->components()[0])
			return false;
		return subtreeFoldable(*tuple->components()[0]);
	}
	if (auto const* unary = dynamic_cast<UnaryOperation const*>(&_e))
	{
		auto op = unary->getOperator();
		if (op != solidity::langutil::Token::Sub && op != solidity::langutil::Token::BitNot)
			return false;
		return subtreeFoldable(unary->subExpression());
	}
	if (auto const* binary = dynamic_cast<BinaryOperation const*>(&_e))
	{
		using solidity::langutil::Token;
		switch (binary->getOperator())
		{
		case Token::Add: case Token::Sub: case Token::Mul: case Token::Div:
		case Token::Mod: case Token::Exp: case Token::SHL: case Token::SAR:
		case Token::BitAnd: case Token::BitOr: case Token::BitXor:
			return subtreeFoldable(binary->leftExpression())
				&& subtreeFoldable(binary->rightExpression());
		default:
			return false;
		}
	}
	// Conversions, calls, ternaries, index/member accesses: no fold.
	return false;
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolcConstFold::foldAnnotated(
	Expression const& _expr,
	TypeMapper& _typeMapper,
	awst::SourceLocation const& _loc)
{
	auto const* ratType = dynamic_cast<RationalNumberType const*>(_expr.annotation().type);
	if (!ratType || ratType->isFractional())
		return nullptr;
	// Solc folded the whole expression to a non-fractional rational; emit its
	// value via the shared helper (promotes uint64→biguint when the magnitude
	// overflows uint64; negatives arrive as 256-bit two's complement).
	return TypeCoercion::rationalIntConstant(
		ratType->literalValue(nullptr), _typeMapper.map(_expr.annotation().type), _loc);
}

std::shared_ptr<awst::Expression> SolcConstFold::foldTyped(
	Expression const& _expr,
	awst::SourceLocation const& _loc)
{
	auto const* intType = dynamic_cast<IntegerType const*>(_expr.annotation().type);
	if (!intType)
		return nullptr;
	if (!subtreeFoldable(_expr))
		return nullptr;
	auto value = nodeValue(_expr);
	if (!value)
		return nullptr;

	// 256-bit two's complement, the form canonicalIntConstant expects.
	solidity::bigint num = value->numerator();
	if (num < 0)
		num += solidity::bigint(1) << 256;
	return TypeCoercion::canonicalIntConstant(
		solidity::u256(num), intType->numBits(), _loc);
}

std::optional<solidity::u256> SolcConstFold::constantVarEvmWord(
	VariableDeclaration const& _varDecl)
{
	if (!_varDecl.isConstant() || !_varDecl.value())
		return std::nullopt;

	auto const* initExpr = _varDecl.value().get();

	// bytesN declarations view their value left-aligned in the 32-byte word.
	auto applyBytesNShift = [&](solidity::u256 _val) -> solidity::u256 {
		if (auto const* fixedBytes = dynamic_cast<FixedBytesType const*>(_varDecl.type()))
		{
			size_t shiftBits = (32 - fixedBytes->numBytes()) * 8;
			_val <<= shiftBits;
		}
		return _val;
	};

	// Literal fast-path. Two cases ConstantEvaluator can't or won't fold:
	//   - hex literals typed as address/bytesN/etc — TypeProvider::forLiteral
	//     returns AddressType / FixedBytesType, and constantToTypedValue
	//     bails on anything that isn't RationalNumberType/StringLiteralType.
	//   - non-hex string literals — packed left-aligned into a 32-byte word
	//     using EVM's bytesN convention.
	if (auto const* literal = dynamic_cast<Literal const*>(initExpr))
	{
		std::string const& value = literal->value();
		auto const* exprType = initExpr->annotation().type;
		// Bool: ConstantEvaluator's constantToTypedValue handles only
		// RationalNumberType and StringLiteralType, so `bool constant d =
		// true;` falls through to monostate. Map true/false directly.
		if (dynamic_cast<BoolType const*>(exprType))
			return value == "true" ? solidity::u256(1) : solidity::u256(0);
		if (value.size() > 2 && value.substr(0, 2) == "0x")
		{
			try { return applyBytesNShift(solidity::u256(value)); }
			catch (...) {}
		}
		else if (!dynamic_cast<RationalNumberType const*>(exprType))
		{
			// String literal: pack as bytesN (left-aligned). Skip for
			// rational — those are simple numeric values (e.g. "2") that
			// ConstantEvaluator handles correctly below.
			solidity::u256 numVal = 0;
			for (char ch: value)
				numVal = (numVal << 8) | static_cast<unsigned char>(ch);
			size_t shiftBits = (32 - value.size()) * 8;
			numVal <<= shiftBits;
			return numVal;
		}
	}

	// Chained constant: `const bb = b;` where b's value is itself a
	// non-rational literal (address/bytesN/bool) that ConstantEvaluator
	// can't fold to a rational. Recurse via this function so the literal
	// fast-path above kicks in for the leaf, then re-apply the bytesN
	// shift for the outer declaration.
	if (auto const* identifier = dynamic_cast<Identifier const*>(initExpr))
	{
		if (auto const* refDecl = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration))
		{
			if (refDecl->isConstant())
			{
				auto inner = constantVarEvmWord(*refDecl);
				if (inner)
				{
					// Strip the inner bytesN shift (if any) before re-applying
					// the outer one — otherwise chains like `bytes3 cc = c;
					// bytes3 ccc = cc;` would shift twice.
					if (auto const* innerFixedBytes =
						dynamic_cast<FixedBytesType const*>(refDecl->type()))
					{
						size_t innerShift = (32 - innerFixedBytes->numBytes()) * 8;
						*inner >>= innerShift;
					}
					return applyBytesNShift(*inner);
				}
			}
		}
	}

	// Numeric / bool / identifier-chain / arithmetic over constants:
	// solc's ConstantEvaluator handles all of these (including the
	// chained-const case `const aa = a;` and constant binary ops like
	// `const x = 1 << 32;`). It walks the AST itself, so we don't need
	// our own recursion + depth cap.
	auto evaluated = ConstantEvaluator::tryEvaluate(*initExpr);
	if (!std::holds_alternative<rational>(evaluated.value))
		return std::nullopt;

	auto const& rat = std::get<rational>(evaluated.value);
	if (rat.denominator() != 1)
		return std::nullopt;

	return applyBytesNShift(solidity::u256(rat.numerator()));
}

bool SolcConstFold::isEffectFree(Expression const& _expr)
{
	// SetOnce<bool>: set for every expression once the TypeChecker ran (which
	// is before any builder executes). `set()` guards the defensive default —
	// an unset annotation reads as "assume effects" rather than crashing.
	auto const& isPure = _expr.annotation().isPure;
	return isPure.set() && *isPure;
}

} // namespace puyasol::builder
