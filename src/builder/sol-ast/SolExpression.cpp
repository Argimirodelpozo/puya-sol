#include "builder/sol-ast/SolExpression.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder::sol_ast
{

SolExpression::SolExpression(
	eb::BuilderContext& _ctx,
	solidity::frontend::Expression const& _node)
	: m_ctx(_ctx),
	  m_node(_node),
	  m_solType(_node.annotation().type),
	  m_wtype(_ctx.typeMapper.map(_node.annotation().type)),
	  m_loc(_ctx.makeLoc(
		  _node.location().start,
		  _node.location().end))
{
}

} // namespace puyasol::builder::sol_ast
