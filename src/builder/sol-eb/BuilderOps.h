#pragma once

namespace puyasol::builder::eb
{

/// Binary arithmetic/bitwise operators.
enum class BuilderBinaryOp
{
	Add,
	Sub,
	Mult,
	Div,
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
enum class BuilderComparisonOp
{
	Eq,
	Ne,
	Lt,
	Lte,
	Gt,
	Gte,
};

/// Returns the reversed comparison (swaps operands).
inline BuilderComparisonOp reversedOp(BuilderComparisonOp _op)
{
	switch (_op)
	{
	case BuilderComparisonOp::Eq: return BuilderComparisonOp::Eq;
	case BuilderComparisonOp::Ne: return BuilderComparisonOp::Ne;
	case BuilderComparisonOp::Lt: return BuilderComparisonOp::Gt;
	case BuilderComparisonOp::Lte: return BuilderComparisonOp::Gte;
	case BuilderComparisonOp::Gt: return BuilderComparisonOp::Lt;
	case BuilderComparisonOp::Gte: return BuilderComparisonOp::Lte;
	}
	return _op; // unreachable
}

/// Unary operators.
enum class BuilderUnaryOp
{
	Positive,
	Negative,
	BitInvert,
	LogicalNot,
	PreIncrement,
	PreDecrement,
	PostIncrement,
	PostDecrement,
	Delete,
};

} // namespace puyasol::builder::eb
