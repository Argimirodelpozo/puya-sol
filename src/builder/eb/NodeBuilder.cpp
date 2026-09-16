#include "builder/eb/NodeBuilder.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

NodeBuilder::NodeBuilder(ContractContext& _ctx)
	: m_ctx(_ctx), m_scope(_ctx.scope())
{
}

// ─────────────────────────────────────────────────────────────────────
// InstanceBuilder defaults
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> InstanceBuilder::resolve_lvalue()
{
	return resolve(); // default: not an lvalue; override in builders that support assignment
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

} // namespace puyasol::builder::eb
