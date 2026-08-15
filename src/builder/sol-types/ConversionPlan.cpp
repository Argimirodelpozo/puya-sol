#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> ConversionPlan::emit(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	char const* site = "implicit conversion";
	switch (m_context)
	{
	case Context::Assignment: site = "assignment"; break;
	case Context::Initialization: site = "variable-declaration init"; break;
	case Context::Argument: site = "internal-call arg"; break;
	case Context::Return: site = "return"; break;
	}
	TypeCoercion::assertImplicitlyConvertible(m_source, m_target, _loc, site);
	_value = TypeCoercion::coerceForAssignment(
		std::move(_value), m_targetRepresentation, _loc);
	return TypeCoercion::signExtendSignedWiden(
		std::move(_value), m_source, m_target, _loc);
}

} // namespace puyasol::builder
