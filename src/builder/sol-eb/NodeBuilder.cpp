#include "builder/sol-eb/NodeBuilder.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

// ─────────────────────────────────────────────────────────────────────
// InstanceBuilder defaults
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> InstanceBuilder::resolve_lvalue()
{
	// Most expressions are not lvalues — override in builders that support assignment.
	return resolve();
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::unary_op(
	BuilderUnaryOp /*_op*/, awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // not implemented — caller should report error
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::binary_op(
	InstanceBuilder& /*_other*/, BuilderBinaryOp /*_op*/,
	awst::SourceLocation const& /*_loc*/, bool /*_reverse*/)
{
	return nullptr; // not implemented — caller tries reverse dispatch
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::compare(
	InstanceBuilder& /*_other*/, BuilderComparisonOp /*_op*/,
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // not implemented — caller tries reversed comparison
}

std::shared_ptr<awst::Statement> InstanceBuilder::augmented_assignment(
	BuilderBinaryOp /*_op*/, InstanceBuilder& /*_rhs*/,
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // not supported
}

std::unique_ptr<NodeBuilder> InstanceBuilder::member_access(
	std::string const& _name, awst::SourceLocation const& /*_loc*/)
{
	Logger::instance().warning("unrecognised member '" + _name + "' on type " +
		(m_expr && m_expr->wtype ? m_expr->wtype->name() : "unknown"));
	return nullptr;
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::index(
	InstanceBuilder& /*_idx*/, awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // type does not support indexing
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::call(
	std::vector<std::shared_ptr<awst::Expression>>& /*_args*/,
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // not callable
}

std::shared_ptr<awst::Expression> InstanceBuilder::to_bytes(
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // cannot convert to bytes
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::bool_eval(
	awst::SourceLocation const& /*_loc*/, bool /*_negate*/)
{
	// Default: treat as truthy. Concrete builders override for proper bool evaluation.
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// TypeBuilder defaults
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeBuilder::try_convert(
	std::shared_ptr<awst::Expression> /*_expr*/,
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // conversion not supported by default
}

std::unique_ptr<NodeBuilder> TypeBuilder::member_access(
	std::string const& _name, awst::SourceLocation const& /*_loc*/)
{
	Logger::instance().warning("unrecognised member '" + _name + "' on type expression");
	return nullptr;
}

std::unique_ptr<InstanceBuilder> TypeBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool /*_negate*/)
{
	// Type expressions are always truthy.
	auto bc = awst::makeBoolConstant(true, _loc);
	// We can't construct an InstanceBuilder here yet (no concrete bool builder).
	// This will be wired up in Phase 1 when BoolBuilder exists.
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// CallableBuilder defaults
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<NodeBuilder> CallableBuilder::member_access(
	std::string const& _name, awst::SourceLocation const& /*_loc*/)
{
	Logger::instance().warning("no member '" + _name + "' on callable");
	return nullptr;
}

std::unique_ptr<InstanceBuilder> CallableBuilder::bool_eval(
	awst::SourceLocation const& /*_loc*/, bool /*_negate*/)
{
	// Callables are always truthy (they exist).
	return nullptr;
}

} // namespace puyasol::builder::eb
