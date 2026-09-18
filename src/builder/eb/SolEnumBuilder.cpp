/// @file SolEnumBuilder.cpp
/// Solidity enum type builder — full numeric words until range validation.

#include "builder/eb/SolEnumBuilder.h"
#include "builder/eb/SolBoolBuilder.h"
#include "builder/types/TypeCoercion.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolEnumBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (!dynamic_cast<solidity::frontend::EnumType const*>(_other.solType()))
		return nullptr;

	auto lhs = TypeCoercion::checkedEnum(resolve(), m_enumType, _loc, &m_ctx.preEffects());
	auto rhs = TypeCoercion::checkedEnum(_other.resolve(), _other.solType(), _loc, &m_ctx.preEffects());

	auto e = awst::makeNumericCompare(std::move(lhs), _op, std::move(rhs), _loc);
	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(e));
}

} // namespace puyasol::builder::eb
