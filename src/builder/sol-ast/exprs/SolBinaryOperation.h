#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Binary operations: arithmetic, comparison, bitwise, boolean.
/// Handles user-defined operator overloading, constant folding, sol-eb builder
/// dispatch, and falls back to eb::buildBinaryOp for the rest.
class SolBinaryOperation: public SolExpression
{
public:
	SolBinaryOperation(eb::ContractContext& _ctx, solidity::frontend::BinaryOperation const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::BinaryOperation const& m_binOp;

	/// Handle user-defined operator overloading.
	std::shared_ptr<awst::Expression> tryUserDefinedOp();
	/// Handle compile-time constant folding.
	std::shared_ptr<awst::Expression> tryConstantFold();
	/// Short-circuit && / || whose RHS has side effects (pre-statements, e.g. a checked op that can
	/// revert): gate those side effects behind the condition so they run only when the RHS is reached
	/// (`b != 0 && a / b > x` must not divide when b == 0). Returns nullptr for non-&&/|| or a
	/// side-effect-free RHS (the plain boolean lowering then applies). Mirrors the ternary (SolConditional).
	std::shared_ptr<awst::Expression> trySolShortCircuit();
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

	/// Handle signed integer exponentiation.
	/// Computes abs(base)^exp, negates if base negative and exp odd.
	std::shared_ptr<awst::Expression> buildSignedExp(
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		solidity::frontend::IntegerType const* _intType);

	/// Handle signed integer division and modulo.
	/// Uses absolute values with sign correction.
	std::shared_ptr<awst::Expression> buildSignedDivMod(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		solidity::frontend::IntegerType const* _intType);
};

} // namespace puyasol::builder::sol_ast
