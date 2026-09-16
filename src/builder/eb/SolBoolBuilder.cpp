/// @file SolBoolBuilder.cpp
/// Solidity bool type builder.

#include "builder/eb/SolBoolBuilder.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

solidity::frontend::Type const* SolBoolBuilder::solType() const
{
	return solidity::frontend::TypeProvider::boolean();
}

std::unique_ptr<InstanceBuilder> SolBoolBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (_other.wtype() != awst::WType::boolType())
		return nullptr;
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	auto e = awst::makeNumericCompare(resolve(), (_op == BuilderComparisonOp::Eq)
		? awst::NumericComparison::Eq
		: awst::NumericComparison::Ne, _other.resolve(), _loc);
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolBoolBuilder::unary_op(
	BuilderUnaryOp _op, awst::SourceLocation const& _loc)
{
	if (_op != BuilderUnaryOp::LogicalNot)
		return nullptr;

	auto e = awst::makeNot(resolve(), _loc);
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
}

} // namespace puyasol::builder::eb
