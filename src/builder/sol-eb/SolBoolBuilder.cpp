/// @file SolBoolBuilder.cpp
/// Solidity bool type builder.

#include "builder/sol-eb/SolBoolBuilder.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

solidity::frontend::Type const* SolBoolBuilder::solType() const
{
	return solidity::frontend::TypeProvider::boolean();
}

std::unique_ptr<InstanceBuilder> SolBoolBuilder::binary_op(
	InstanceBuilder& _other, BuilderBinaryOp _op,
	awst::SourceLocation const& _loc, bool /*_reverse*/)
{
	// Bool only supports logical And/Or (both && and & map here for bools)
	if (_other.wtype() != awst::WType::boolType())
		return nullptr;

	awst::BinaryBooleanOperator boolOp;
	switch (_op)
	{
	case BuilderBinaryOp::BitAnd:
		boolOp = awst::BinaryBooleanOperator::And;
		break;
	case BuilderBinaryOp::BitOr:
		boolOp = awst::BinaryBooleanOperator::Or;
		break;
	default:
		return nullptr;
	}

	auto e = std::make_shared<awst::BooleanBinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->left = resolve();
	e->op = boolOp;
	e->right = _other.resolve();
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
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

	auto e = std::make_shared<awst::Not>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->expr = resolve();
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolBoolBuilder::bool_eval(
	awst::SourceLocation const& /*_loc*/, bool _negate)
{
	if (_negate)
	{
		auto e = std::make_shared<awst::Not>();
		e->sourceLocation = m_expr->sourceLocation;
		e->wtype = awst::WType::boolType();
		e->expr = resolve();
		return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
	}
	return std::make_unique<SolBoolBuilder>(m_ctx, resolve());
}

} // namespace puyasol::builder::eb
