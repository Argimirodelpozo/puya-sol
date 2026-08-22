#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder::sol_ast
{

/// Tuple expressions, inline array literals, and parenthesization.
class SolTupleExpression: public SolExpression
{
public:
	SolTupleExpression(eb::ContractContext& _ctx, solidity::frontend::TupleExpression const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::TupleExpression const& m_tuple;
};

} // namespace puyasol::builder::sol_ast
