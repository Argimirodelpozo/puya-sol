#include "builder/sol-ast/SolExpression.h"
#include "builder/sol-types/TypeMapper.h"

#include <cassert>

namespace puyasol::builder::sol_ast
{

namespace {
// Null currentScope means the call site forgot to push a scope before
// building expressions — assert to catch that at the entry point.
Context& currentScopeOrAbort(eb::ContractContext& _ctx)
{
	assert(_ctx.currentScope && "expression visitor created with no current scope");
	return *_ctx.currentScope;
}
}

SolExpression::SolExpression(
	eb::ContractContext& _ctx,
	solidity::frontend::Expression const& _node)
	: m_ctx(_ctx),
	  m_scope(currentScopeOrAbort(_ctx)),
	  m_node(_node),
	  m_solType(_node.annotation().type),
	  m_wtype(_ctx.typeMapper.map(_node.annotation().type)),
	  m_loc(_ctx.makeLoc(
		  _node.location().start,
		  _node.location().end))
{
}

} // namespace puyasol::builder::sol_ast
