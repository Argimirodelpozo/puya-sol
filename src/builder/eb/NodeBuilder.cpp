#include "builder/eb/NodeBuilder.h"

namespace puyasol::builder::eb
{

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
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr;
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::compare(
	InstanceBuilder& /*_other*/, BuilderComparisonOp /*_op*/,
	awst::SourceLocation const& /*_loc*/)
{
	return nullptr;
}

std::unique_ptr<InstanceBuilder> InstanceBuilder::index(
	InstanceBuilder& /*_idx*/, awst::SourceLocation const& /*_loc*/)
{
	return nullptr; // type does not support indexing
}

} // namespace puyasol::builder::eb
