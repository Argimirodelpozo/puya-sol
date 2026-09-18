#pragma once

#include "builder/context/TranslationContext.h"
#include "builder/context/ContractContext.h"
#include "builder/eb/BuilderOps.h"
#include "awst/Node.h"
#include "awst/WType.h"

#include "builder/solc/SolcFwd.h"

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder::eb
{

// ─────────────────────────────────────────────────────────────────────
// InstanceBuilder — wraps a resolved Expression + its Solidity type
// ─────────────────────────────────────────────────────────────────────

class InstanceBuilder
{
public:
	virtual ~InstanceBuilder() = default;

	/// The Solidity type this builder was created from.
	virtual solidity::frontend::Type const* solType() const = 0;

	/// The AWST type this builder produces.
	virtual awst::WType const* wtype() const { return m_expr ? m_expr->wtype : nullptr; }

	/// Get the underlying AWST expression.
	virtual std::shared_ptr<awst::Expression> resolve() { return m_expr; }

	/// Get the expression as an assignment target (lvalue).
	virtual std::shared_ptr<awst::Expression> resolve_lvalue();

	// ── Operators ──

	/// Handle unary operation on this value.
	/// Returns nullptr if unsupported.
	virtual std::unique_ptr<InstanceBuilder> unary_op(
		BuilderUnaryOp _op, awst::SourceLocation const& _loc);

	/// Handle `this {op} other`. Returns nullptr if unsupported.
	virtual std::unique_ptr<InstanceBuilder> binary_op(
		InstanceBuilder& _other, BuilderBinaryOp _op,
		awst::SourceLocation const& _loc);

	/// Handle `this {cmp} other`.
	/// Returns nullptr if unsupported.
	virtual std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc);

	virtual std::unique_ptr<InstanceBuilder> index(
		InstanceBuilder& _idx, awst::SourceLocation const& _loc);

protected:
	ContractContext& m_ctx;
	/// Innermost scope at construction, including its checked/unchecked mode.
	sol_ast::Context& m_scope;
	std::shared_ptr<awst::Expression> m_expr;

	InstanceBuilder(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr
	)
		: m_ctx(_ctx), m_scope(_ctx.scope()), m_expr(std::move(_expr))
	{
	}
};

} // namespace puyasol::builder::eb
