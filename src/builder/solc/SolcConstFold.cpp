/// @file SolcConstFold.cpp
/// See SolcConstFold.h — the one place builders get compile-time constants.

#include "builder/solc/SolcConstFold.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"

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

	if (SolcFacts::expressionAs<Literal>(&_e))
		return true;
	if (auto const* id = SolcFacts::expressionAs<Identifier>(&_e))
	{
		// A constant variable's VALUE (computed + range-checked above) is what
		// solc itself inlines at references — its initializer's internals are
		// solc's business, not re-validated here.
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
			id->annotation().referencedDeclaration);
		return varDecl && varDecl->isConstant();
	}
	if (auto const* unary = SolcFacts::expressionAs<UnaryOperation>(&_e))
	{
		auto op = unary->getOperator();
		if (op != solidity::langutil::Token::Sub && op != solidity::langutil::Token::BitNot)
			return false;
		return subtreeFoldable(unary->subExpression());
	}
	if (auto const* binary = SolcFacts::expressionAs<BinaryOperation>(&_e))
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

	// Let solc distinguish numeric and string literals (including text beginning
	// with "0x") and evaluate constant arithmetic before encoding the EVM word.
	auto evaluated = ConstantEvaluator::tryEvaluate(*initExpr);
	if (auto const* rat = std::get_if<rational>(&evaluated.value))
	{
		if (rat->denominator() != 1)
			return std::nullopt;
		return applyBytesNShift(solidity::u256(rat->numerator()));
	}
	if (auto const* text = std::get_if<std::string>(&evaluated.value))
	{
		if (text->size() > 32)
			return std::nullopt;
		solidity::u256 word = 0;
		for (unsigned char byte: *text)
			word = (word << 8) | byte;
		return word << ((32 - text->size()) * 8);
	}

	// The evaluator does not handle bool/address literals. Their annotated solc
	// types already expose the literal's canonical value; do not parse spelling.
	if (auto const* literal = SolcFacts::expressionAs<Literal>(initExpr))
	{
		auto const* exprType = initExpr->annotation().type;
		if (dynamic_cast<BoolType const*>(exprType)
			|| dynamic_cast<AddressType const*>(exprType))
			return exprType->literalValue(literal);
	}

	// Solc accepted the implicit conversion at this reference. Non-numeric
	// constant chains preserve the canonical word: fixed-bytes widening pads on
	// the RIGHT, so the already left-aligned inner word must not be shifted again.
	if (auto const* identifier = SolcFacts::expressionAs<Identifier>(initExpr))
	{
		if (auto const* refDecl = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration))
			return constantVarEvmWord(*refDecl);
	}
	return std::nullopt;
}

std::optional<solidity::u256> SolcConstFold::constantAddress(Expression const& expression)
{
	using solidity::bigint;
	using solidity::u256;
	if (!dynamic_cast<AddressType const*>(expression.annotation().type)) return std::nullopt;
	std::function<std::optional<bigint>(Expression const&)> value = [&](Expression const& input) -> std::optional<bigint> {
		auto const& source = SolcFacts::unparenthesized(input);
		if (auto const* literal = SolcFacts::expressionAs<Literal>(&source);
			literal && dynamic_cast<AddressType const*>(source.annotation().type))
			return bigint(source.annotation().type->literalValue(literal));
		if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(ASTNode::referencedDeclaration(source));
			declaration && declaration->isConstant() && declaration->value())
			return value(*declaration->value());
		if (auto const* call = SolcFacts::expressionAs<FunctionCall>(&source);
			call && *call->annotation().kind == FunctionCallKind::TypeConversion && call->arguments().size() == 1)
		{
			auto result = value(*call->arguments()[0]);
			if (!result) return std::nullopt;
			if (dynamic_cast<AddressType const*>(source.annotation().type))
				return *result >= 0 && *result < (bigint(1) << 160) ? result : std::nullopt;
			if (auto const* integer = dynamic_cast<IntegerType const*>(source.annotation().type))
				return *result >= integer->minValue() && *result <= integer->maxValue() ? result : std::nullopt;
			return std::nullopt;
		}
		if (subtreeFoldable(source))
			if (auto result = nodeValue(source)) return result->numerator();
		return std::nullopt;
	};
	auto result = value(expression);
	return result && *result >= 0 && *result < (bigint(1) << 160)
		? std::optional<u256>(u256(*result)) : std::nullopt;
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
