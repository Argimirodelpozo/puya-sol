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
	using Arguments = std::vector<std::shared_ptr<solidity::frontend::Expression const>>;

	SolFunctionCall(
		eb::ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	/// The underlying FunctionCall AST node.
	solidity::frontend::FunctionCall const& call() const { return m_call; }

	/// The function arguments. solc's FunctionCall::arguments() returns a vector
	/// by value, so cache that vector for the lifetime of this wrapper rather than
	/// returning a dangling reference to solc's temporary.
	Arguments const& arguments() const
	{
		return m_arguments;
	}

	/// The unwrapped function expression (strips FunctionCallOptions).
	solidity::frontend::Expression const& funcExpression() const;

	/// Extract {value: X} from FunctionCallOptions, or nullptr if not present.
	std::shared_ptr<awst::Expression> extractCallValue();

protected:
	solidity::frontend::FunctionCall const& m_call;
	Arguments m_arguments;
};

} // namespace puyasol::builder::sol_ast
