#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Variable/constant/state variable identifier resolution.
class SolIdentifier: public SolExpression
{
public:
	SolIdentifier(eb::BuilderContext& _ctx, solidity::frontend::Identifier const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::Identifier const& m_ident;
};

} // namespace puyasol::builder::sol_ast
