#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

/// Base class for all FunctionCall expression nodes.
///
/// Provides access to the underlying FunctionCall AST node,
/// its arguments, and the resolved FunctionType::Kind.
class SolFunctionCall: public SolExpression
{
public:
	SolFunctionCall(
		eb::BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	/// The underlying FunctionCall AST node.
	solidity::frontend::FunctionCall const& call() const { return m_call; }

	/// The function arguments.
	std::vector<std::shared_ptr<solidity::frontend::Expression const>> const& arguments() const
	{
		return m_call.arguments();
	}

	/// The unwrapped function expression (strips FunctionCallOptions).
	solidity::frontend::Expression const& funcExpression() const;

	/// Extract {value: X} from FunctionCallOptions, or nullptr if not present.
	std::shared_ptr<awst::Expression> extractCallValue();

protected:
	solidity::frontend::FunctionCall const& m_call;
};

} // namespace puyasol::builder::sol_ast
