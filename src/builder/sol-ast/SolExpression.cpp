#include "builder/sol-ast/SolExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/ConversionPlan.h"

// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

SolExpression::SolExpression(
	eb::ContractContext& _ctx,
	solidity::frontend::Expression const& _node)
	: m_ctx(_ctx),
	  m_scope(_ctx.scope()),
	  m_node(_node),
	  m_solType(_node.annotation().type),
	  m_wtype(_ctx.typeMapper.map(_node.annotation().type)),
	  m_loc(_ctx.makeLoc(_node.location()))
{
}

std::shared_ptr<awst::Expression> SolExpression::buildConstantValue(
	solidity::frontend::VariableDeclaration const& declaration)
{
	auto const& initializer = *declaration.value();
	return ConversionPlan{initializer.annotation().type, declaration.type(),
		m_ctx.typeMapper.map(declaration.type()), ConversionPlan::Context::Initialization}.emit(
			buildExpr(initializer), m_loc, &m_ctx.preEffects());
}

} // namespace puyasol::builder::sol_ast
