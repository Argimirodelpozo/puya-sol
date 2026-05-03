#include "builder/sol-ast/SolExpression.h"
#include "builder/sol-types/TypeMapper.h"

#include <cassert>

namespace puyasol::builder::sol_ast
{

namespace {
// Pull the current scope out of the context. Visitors are only ever
// created during translation, when *some* scope is on the stack — if
// `currentScope` is null, that's a bug at the call site (an entry
// point forgot to push a scope before recursing into expressions).
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
