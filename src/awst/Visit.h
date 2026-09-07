#pragma once

/// @file Visit.h
/// Pre-order traversal of AWST expression trees. The const walker is the
/// real one; the mutable overloads are a const_cast shim over it.

#include "awst/Node.h"

#include <functional>

namespace puyasol::awst
{

using ExpressionVisitor = std::function<void(Expression const&)>;
using MutableExpressionVisitor = std::function<void(Expression&)>;

/// Visit every expression contained by `_statement` in pre-order.
void visitExpressions(Statement const& _statement, ExpressionVisitor const& _visitor);

/// Visit every expression contained by `_method` in pre-order.
void visitExpressions(ContractMethod const& _method, ExpressionVisitor const& _visitor);

void visitExpressions(Statement& _statement, MutableExpressionVisitor const& _visitor);
void visitExpressions(ContractMethod& _method, MutableExpressionVisitor const& _visitor);

} // namespace puyasol::awst
