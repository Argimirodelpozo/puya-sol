/// @file SolEnumBuilder.cpp
/// Solidity enum type builder — enums encoded as uint64 on AVM.

#include "builder/sol-eb/SolEnumBuilder.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolEnumBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	// Enums compare as uint64
	if (_other.wtype() != awst::WType::uint64Type())
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Enum range validation: EVM panics (0x21) when comparing invalid enum values
	if (m_enumType)
	{
		unsigned numMembers = m_enumType->numberOfMembers();
		auto validateEnum = [&](std::shared_ptr<awst::Expression> val) {
			auto maxVal = awst::makeIntegerConstant(std::to_string(numMembers), _loc);

			auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, std::move(maxVal), _loc);

			auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "enum out of range"), _loc);
			m_ctx.prePendingStatements.push_back(std::move(stmt));
		};
		validateEnum(lhs);
		validateEnum(rhs);
	}

	auto e = std::make_shared<awst::NumericComparisonExpression>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->lhs = std::move(lhs);
	e->rhs = std::move(rhs);

	switch (_op)
	{
	case BuilderComparisonOp::Eq: e->op = awst::NumericComparison::Eq; break;
	case BuilderComparisonOp::Ne: e->op = awst::NumericComparison::Ne; break;
	case BuilderComparisonOp::Lt: e->op = awst::NumericComparison::Lt; break;
	case BuilderComparisonOp::Lte: e->op = awst::NumericComparison::Lte; break;
	case BuilderComparisonOp::Gt: e->op = awst::NumericComparison::Gt; break;
	case BuilderComparisonOp::Gte: e->op = awst::NumericComparison::Gte; break;
	}
	return std::make_unique<SolEnumBuilder>(m_ctx, m_enumType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolEnumBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto zero = awst::makeIntegerConstant("0", _loc);

	auto cmp = awst::makeNumericCompare(resolve(), _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne, std::move(zero), _loc);
	return std::make_unique<SolEnumBuilder>(m_ctx, m_enumType, std::move(cmp));
}

} // namespace puyasol::builder::eb
