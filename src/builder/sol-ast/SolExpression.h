#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <memory>

namespace puyasol::builder::sol_ast
{

/// Base class for Solidity AST expression nodes in our frontend.
///
/// Each subclass models a specific Solidity expression kind and knows
/// how to translate itself to AWST via toAwst(). The Solidity compiler's
/// type annotations are available through solType() and the AWST target
/// type through wtype().
///
/// Dispatch: SolExpressionFactory creates the right subclass based on
/// FunctionCallKind, FunctionType::Kind, and Type::Category.
class SolExpression
{
public:
	virtual ~SolExpression() = default;

	/// Translate this Solidity expression to an AWST expression.
	virtual std::shared_ptr<awst::Expression> toAwst() = 0;

	/// The Solidity type of this expression (from annotation).
	solidity::frontend::Type const* solType() const { return m_solType; }

	/// The AWST target type (mapped from Solidity type).
	awst::WType const* wtype() const { return m_wtype; }

	/// Source location in the original Solidity file.
	awst::SourceLocation const& loc() const { return m_loc; }

protected:
	SolExpression(
		eb::BuilderContext& _ctx,
		solidity::frontend::Expression const& _node);

	eb::BuilderContext& m_ctx;
	solidity::frontend::Expression const& m_node;
	solidity::frontend::Type const* m_solType;
	awst::WType const* m_wtype;
	awst::SourceLocation m_loc;

	/// Build a child expression (routes through BuilderContext).
	std::shared_ptr<awst::Expression> buildExpr(
		solidity::frontend::Expression const& _expr)
	{
		return m_ctx.buildExpr(_expr);
	}
};

} // namespace puyasol::builder::sol_ast
