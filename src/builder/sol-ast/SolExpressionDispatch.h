#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>

namespace puyasol::builder::sol_ast
{

/// Build a Solidity expression into AWST by dispatching to the right sol-ast wrapper.
/// This replaces ExpressionBuilder's visitor pattern with direct dynamic_cast dispatch.
std::shared_ptr<awst::Expression> buildExpression(
	eb::BuilderContext& _ctx,
	solidity::frontend::Expression const& _expr);

} // namespace puyasol::builder::sol_ast
