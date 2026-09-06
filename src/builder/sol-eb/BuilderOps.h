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
