#pragma once

#include "builder/ast/SolExpression.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder::sol_ast
{

/// Tuple expressions, inline array literals, and parenthesization.
class SolTupleExpression: public SolExpression
{
public:
	SolTupleExpression(eb::ContractContext& _ctx, solidity::frontend::TupleExpression const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;
	/// Literal tuple bindings transport existing memory pointers, not copies.
	static std::shared_ptr<awst::Expression> buildBindingRhs(
		eb::ContractContext& _ctx, solidity::frontend::Expression const& _expression,
		std::vector<solidity::frontend::VariableDeclaration const*> const& _bindings);

private:
	std::shared_ptr<awst::Expression> buildTuple(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _bindings,
		bool _storageReferences = false);
	solidity::frontend::TupleExpression const& m_tuple;
};

} // namespace puyasol::builder::sol_ast
