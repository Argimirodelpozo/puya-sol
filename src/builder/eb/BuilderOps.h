#pragma once

#include "awst/Node.h"

#include <liblangutil/Token.h>
#include <optional>

namespace puyasol::builder::eb
{

/// Binary arithmetic/bitwise operators.
enum class BuilderBinaryOp
{
	Add,
	Sub,
	Mult,
	FloorDiv,
	Mod,
	Pow,
	LShift,
	RShift,
	BitOr,
	BitXor,
	BitAnd,
};

/// Comparison operators.
using BuilderComparisonOp = awst::NumericComparison;

/// Unary operators.
enum class BuilderUnaryOp
{
	Negative,
	BitInvert,
	LogicalNot,
};

using solidity::langutil::Token;

inline Token binaryToken(Token token)
{
	return solidity::langutil::TokenTraits::isAssignmentOp(token) && token != Token::Assign
		? solidity::langutil::TokenTraits::AssignmentToBinaryOp(token) : token;
}

inline std::optional<BuilderComparisonOp> comparisonOpFor(Token solOp)
{
	switch (solOp)
	{
	case Token::Equal:              return BuilderComparisonOp::Eq;
	case Token::NotEqual:           return BuilderComparisonOp::Ne;
	case Token::LessThan:           return BuilderComparisonOp::Lt;
	case Token::LessThanOrEqual:    return BuilderComparisonOp::Lte;
	case Token::GreaterThan:        return BuilderComparisonOp::Gt;
	case Token::GreaterThanOrEqual: return BuilderComparisonOp::Gte;
	default: return std::nullopt;
	}
}

inline std::optional<BuilderBinaryOp> binaryOpFor(Token solOp)
{
	switch (binaryToken(solOp))
	{
	case Token::Add: return BuilderBinaryOp::Add;
	case Token::Sub: return BuilderBinaryOp::Sub;
	case Token::Mul: return BuilderBinaryOp::Mult;
	case Token::Div: return BuilderBinaryOp::FloorDiv;
	case Token::Mod: return BuilderBinaryOp::Mod;
	case Token::Exp: return BuilderBinaryOp::Pow;
	case Token::SHL: return BuilderBinaryOp::LShift;
	case Token::SHR: case Token::SAR:
		return BuilderBinaryOp::RShift;
	case Token::BitOr: return BuilderBinaryOp::BitOr;
	case Token::BitXor: return BuilderBinaryOp::BitXor;
	case Token::BitAnd: return BuilderBinaryOp::BitAnd;
	default: return std::nullopt;
	}
}

} // namespace puyasol::builder::eb
