/// @file ConditionalBuilder.cpp
/// Handles ternary conditional expressions (a ? b : c).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::Conditional const& _node)
{
	auto loc = makeLoc(_node.location());
	auto e = std::make_shared<awst::ConditionalExpression>();
	e->sourceLocation = loc;
	e->condition = build(_node.condition());
	e->trueExpr = build(_node.trueExpression());
	e->falseExpr = build(_node.falseExpression());
	e->wtype = m_typeMapper.map(_node.annotation().type);
	// Ensure both branches match the result type (e.g., uint64 literal → biguint)
	e->trueExpr = implicitNumericCast(std::move(e->trueExpr), e->wtype, loc);
	e->falseExpr = implicitNumericCast(std::move(e->falseExpr), e->wtype, loc);
	push(e);
	return false;
}

} // namespace puyasol::builder
