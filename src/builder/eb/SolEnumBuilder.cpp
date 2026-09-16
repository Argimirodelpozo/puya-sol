/// @file SolEnumBuilder.cpp
/// Solidity enum type builder — full numeric words until range validation.

#include "builder/eb/SolEnumBuilder.h"
#include "builder/eb/SolBoolBuilder.h"
#include "awst/NameGen.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolEnumBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (!dynamic_cast<solidity::frontend::EnumType const*>(_other.solType()))
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
			auto tmpVar = awst::makeVarExpression(tmpName, val->wtype, _loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(tmpVar, std::move(val), _loc));
			m_ctx.preEffects().push_back(
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
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
}

} // namespace puyasol::builder::eb
