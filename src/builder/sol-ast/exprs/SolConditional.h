#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Ternary conditional: a ? b : c.
class SolConditional: public SolExpression
{
public:
	SolConditional(eb::BuilderContext& _ctx, solidity::frontend::Conditional const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::Conditional const& m_conditional;
};

} // namespace puyasol::builder::sol_ast
