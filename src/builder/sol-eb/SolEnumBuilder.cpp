/// @file SolEnumBuilder.cpp
/// Solidity enum type builder — enums encoded as uint64 on AVM.

#include "builder/sol-eb/SolEnumBuilder.h"
#include "awst/NameGen.h"

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

	// Enum range check: panic(0x21) on out-of-range. Spill to temp so assert and
	// comparison share ONE evaluation (side-effecting `bump()==E.B` otherwise runs twice).
	if (m_enumType)
	{
		unsigned numMembers = m_enumType->numberOfMembers();
		auto spillAndValidate = [&](std::shared_ptr<awst::Expression> val)
			-> std::shared_ptr<awst::Expression> {
			std::string tmpName = "__enum_cmp_" + std::to_string(
				awst::NameGen::next("SolEnumBuilder.compare"));
			auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
			m_ctx.prePendingStatements.push_back(
				awst::makeAssignmentStatement(tmpVar, std::move(val), _loc));
			m_ctx.prePendingStatements.push_back(
				awst::makeExpressionStatement(
					awst::makeEnumRangeAssert(tmpVar, numMembers, _loc), _loc));
			return tmpVar;
		};
		lhs = spillAndValidate(std::move(lhs));
		rhs = spillAndValidate(std::move(rhs));
	}

	awst::NumericComparison cmpOp = awst::NumericComparison::Eq;
	switch (_op)
	{
	case BuilderComparisonOp::Eq: cmpOp = awst::NumericComparison::Eq; break;
	case BuilderComparisonOp::Ne: cmpOp = awst::NumericComparison::Ne; break;
	case BuilderComparisonOp::Lt: cmpOp = awst::NumericComparison::Lt; break;
	case BuilderComparisonOp::Lte: cmpOp = awst::NumericComparison::Lte; break;
	case BuilderComparisonOp::Gt: cmpOp = awst::NumericComparison::Gt; break;
	case BuilderComparisonOp::Gte: cmpOp = awst::NumericComparison::Gte; break;
	}
	auto e = awst::makeNumericCompare(std::move(lhs), cmpOp, std::move(rhs), _loc);
	return std::make_unique<SolEnumBuilder>(m_ctx, m_enumType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolEnumBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto zero = awst::makeZero(_loc);

	auto cmp = awst::makeNumericCompare(resolve(), _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne, std::move(zero), _loc);
	return std::make_unique<SolEnumBuilder>(m_ctx, m_enumType, std::move(cmp));
}

} // namespace puyasol::builder::eb
