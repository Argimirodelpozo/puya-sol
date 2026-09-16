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
// NodeBuilder — abstract root of the builder hierarchy
// ─────────────────────────────────────────────────────────────────────

class NodeBuilder
{
public:
	virtual ~NodeBuilder() = default;

	/// The Solidity type this builder was created from.
	virtual solidity::frontend::Type const* solType() const = 0;

	/// The AWST type this builder produces.
	virtual awst::WType const* wtype() const = 0;

	/// Handle `.member` access. Returns a new builder for the member.
	virtual std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) = 0;

protected:
	ContractContext& m_ctx;
	/// Innermost scope at construction; captured so builders can call
	/// m_scope.isUnchecked() etc. without going through ContractContext.
	sol_ast::Context& m_scope;
	explicit NodeBuilder(ContractContext& _ctx);
};

// ─────────────────────────────────────────────────────────────────────
// InstanceBuilder — wraps a resolved Expression + its Solidity type
// ─────────────────────────────────────────────────────────────────────

class InstanceBuilder: public NodeBuilder
{
public:
	/// Get the underlying AWST expression.
	virtual std::shared_ptr<awst::Expression> resolve() { return m_expr; }

	/// Get the expression as an assignment target (lvalue).
	virtual std::shared_ptr<awst::Expression> resolve_lvalue();

	// ── Operators ──

	/// Handle unary operation on this value.
	/// Returns nullptr if unsupported.
	virtual std::unique_ptr<InstanceBuilder> unary_op(
		BuilderUnaryOp _op, awst::SourceLocation const& _loc);

	/// Handle `this {op} other` (or `other {op} this` if _reverse).
	/// Returns nullptr for "not implemented" — caller tries reverse dispatch.
	virtual std::unique_ptr<InstanceBuilder> binary_op(
		InstanceBuilder& _other, BuilderBinaryOp _op,
		awst::SourceLocation const& _loc, bool _reverse = false);

	/// Handle `this {cmp} other`.
	/// Returns nullptr for "not implemented" — caller tries reversed comparison.
	virtual std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc);

	// ── Member / Index ──

	std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) override;

	virtual std::unique_ptr<InstanceBuilder> index(
		InstanceBuilder& _idx, awst::SourceLocation const& _loc);

	awst::WType const* wtype() const override { return m_expr ? m_expr->wtype : nullptr; }

protected:
	std::shared_ptr<awst::Expression> m_expr;

	InstanceBuilder(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr
	)
		: NodeBuilder(_ctx), m_expr(std::move(_expr))
	{
	}
};

} // namespace puyasol::builder::eb
