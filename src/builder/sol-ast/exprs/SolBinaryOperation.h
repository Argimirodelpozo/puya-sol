#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Binary operations: arithmetic, comparison, bitwise, boolean.
/// Handles user-defined operator overloading, constant folding, sol-eb builder
/// dispatch, and falls back to ExpressionBuilder::buildBinaryOp for the rest.
class SolBinaryOperation: public SolExpression
{
public:
	SolBinaryOperation(eb::BuilderContext& _ctx, solidity::frontend::BinaryOperation const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::BinaryOperation const& m_binOp;

	/// Handle user-defined operator overloading.
	std::shared_ptr<awst::Expression> tryUserDefinedOp();
	/// Handle compile-time constant folding.
	std::shared_ptr<awst::Expression> tryConstantFold();
	/// Try sol-eb builder dispatch for comparison operators.
	std::shared_ptr<awst::Expression> trySolEbDispatch(
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right);

	/// Handle checked signed integer arithmetic (add, sub, mul).
	/// Wraps mod 2^N and adds signed overflow detection.
	std::shared_ptr<awst::Expression> buildSignedArithmetic(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		solidity::frontend::IntegerType const* _intType);
};

} // namespace puyasol::builder::sol_ast
