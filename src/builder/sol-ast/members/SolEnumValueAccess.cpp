#include "builder/sol-ast/members/SolEnumValueAccess.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolEnumValueAccess::toAwst()
{
	auto const* enumVal = dynamic_cast<solidity::frontend::EnumValue const*>(
		m_memberAccess.annotation().referencedDeclaration);
	if (!enumVal) return nullptr;

	// Find this value's ordinal. (EnumType::memberValue(name) would work
	// but requires constructing an EnumType we don't have a handle to here.)
	auto const* enumDef = dynamic_cast<solidity::frontend::EnumDefinition const*>(
		enumVal->scope());
	if (!enumDef) return nullptr;

	int index = 0;
	for (auto const& member: enumDef->members())
	{
		if (member.get() == enumVal)
			break;
		++index;
	}

	return awst::makeIntegerConstant(index, m_loc);
}

} // namespace puyasol::builder::sol_ast
