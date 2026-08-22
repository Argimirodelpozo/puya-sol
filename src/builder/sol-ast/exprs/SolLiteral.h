#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder::sol_ast
{

/// Number, bool, string, and hex literal expressions.
class SolLiteral: public SolExpression
{
public:
	SolLiteral(eb::ContractContext& _ctx, solidity::frontend::Literal const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::Literal const& m_literal;
};

} // namespace puyasol::builder::sol_ast
