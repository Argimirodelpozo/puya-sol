#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-eb/BuilderOps.h"
#include "awst/Node.h"
#include "awst/WType.h"

#include <libsolidity/ast/Types.h>

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

	/// The Solidity type this builder was created from (nullptr for callables).
	virtual solidity::frontend::Type const* solType() const = 0;

	/// The AWST type this builder produces (nullptr for callables/type exprs).
	virtual awst::WType const* wtype() const = 0;

	/// Handle `.member` access. Returns a new builder for the member.
	virtual std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) = 0;

	/// Evaluate as a boolean condition (for if/while/ternary).
	/// If _negate is true, return the negated bool.
	virtual std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) = 0;

protected:
	BuilderContext& m_ctx;
	explicit NodeBuilder(BuilderContext& _ctx): m_ctx(_ctx) {}
};

// ─────────────────────────────────────────────────────────────────────
// InstanceBuilder — wraps a resolved Expression + its Solidity type
//
// Each concrete builder stores the Solidity type it was created from
// and uses it to determine semantics (signedness, bit width, etc.)
// while producing the correct AWST nodes.
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

	/// Handle `this {op}= rhs`. Returns the assignment statement, or nullptr.
	virtual std::shared_ptr<awst::Statement> augmented_assignment(
		BuilderBinaryOp _op, InstanceBuilder& _rhs,
		awst::SourceLocation const& _loc);

	// ── Member / Index / Call ──

	std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) override;

	virtual std::unique_ptr<InstanceBuilder> index(
		InstanceBuilder& _idx, awst::SourceLocation const& _loc);

	virtual std::unique_ptr<InstanceBuilder> call(
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	// ── Conversion ──

	virtual std::shared_ptr<awst::Expression> to_bytes(
		awst::SourceLocation const& _loc);

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

	awst::WType const* wtype() const override { return m_expr ? m_expr->wtype : nullptr; }

protected:
	std::shared_ptr<awst::Expression> m_expr;

	InstanceBuilder(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: NodeBuilder(_ctx), m_expr(std::move(_expr))
	{
	}
};

// ─────────────────────────────────────────────────────────────────────
// TypeBuilder — for type expressions: handles construction/conversion
// ─────────────────────────────────────────────────────────────────────

class TypeBuilder: public NodeBuilder
{
public:
	virtual awst::WType const* produces() const = 0;

	virtual std::unique_ptr<InstanceBuilder> construct(
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc) = 0;

	virtual std::unique_ptr<InstanceBuilder> try_convert(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc);

	solidity::frontend::Type const* solType() const override { return nullptr; }
	awst::WType const* wtype() const override { return nullptr; }

	std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

protected:
	TypeBuilder(BuilderContext& _ctx): NodeBuilder(_ctx) {}
};

// ─────────────────────────────────────────────────────────────────────
// CallableBuilder — for callable things (free functions, builtins)
// ─────────────────────────────────────────────────────────────────────

class CallableBuilder: public NodeBuilder
{
public:
	solidity::frontend::Type const* solType() const override { return nullptr; }
	awst::WType const* wtype() const override { return nullptr; }

	virtual std::unique_ptr<InstanceBuilder> call(
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc) = 0;

	std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

protected:
	CallableBuilder(BuilderContext& _ctx): NodeBuilder(_ctx) {}
};

} // namespace puyasol::builder::eb
