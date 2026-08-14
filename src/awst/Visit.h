#pragma once

/// @file Visit.h
/// Read-only traversal of AWST expression trees. Production analysis and
/// validation must not depend on the experimental splitter's mutating walker.

#include "awst/Node.h"

#include <functional>

namespace puyasol::awst
{

using ExpressionVisitor = std::function<void(Expression const&)>;
using MutableExpressionVisitor = std::function<void(Expression&)>;

/// Visit `_expression` and every descendant expression in pre-order.
void visitExpressions(Expression const& _expression, ExpressionVisitor const& _visitor);

/// Visit every expression contained by `_statement` in pre-order.
void visitExpressions(Statement const& _statement, ExpressionVisitor const& _visitor);

/// Visit every expression contained by `_method` in pre-order.
void visitExpressions(ContractMethod const& _method, ExpressionVisitor const& _visitor);

/// Mutable-value traversal. The tree shape and owning pointers stay fixed;
/// callers may update fields on the visited expression nodes.
void visitExpressions(Expression& _expression, MutableExpressionVisitor const& _visitor);
void visitExpressions(Statement& _statement, MutableExpressionVisitor const& _visitor);
void visitExpressions(ContractMethod& _method, MutableExpressionVisitor const& _visitor);

} // namespace puyasol::awst
